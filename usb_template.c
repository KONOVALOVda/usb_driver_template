// SPDX-License-Identifier: GPL-2.0
// Указание лицензии — обязательно для кода, который может попасть в основное дерево ядра

/*
 * USB Skeleton driver - 2.2
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 * Это классический шаблон-драйвер для USB-устройств.
 * Используется как стартовая точка при написании собственных USB-драйверов.
 */

#include <linux/kernel.h>           // базовые макросы ядра, printk и т.п.
#include <linux/errno.h>            // коды ошибок (-ENOMEM, -ENODEV и др.)
#include <linux/slab.h>             // kzalloc, kfree
#include <linux/module.h>           // MODULE_LICENSE, module_init и т.д.
#include <linux/kref.h>             // счётчик ссылок kref (управление временем жизни структуры)
#include <linux/uaccess.h>          // copy_to_user, copy_from_user
#include <linux/usb.h>              // всё, что связано с USB: usb_device, urb, usb_driver и т.д.
#include <linux/mutex.h>            // мьютексы для синхронизации


/* Эти значения нужно заменить на реальные VID и PID вашего устройства */
#define USB_SKEL_VENDOR_ID    0xfff0    // Vendor ID — идентификатор производителя
#define USB_SKEL_PRODUCT_ID   0xfff0    // Product ID — идентификатор конкретного устройства

// Таблица устройств, с которыми умеет работать этот драйвер
static const struct usb_device_id skel_table[] = {
    { USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },  // совпадение по VID:PID
    { }                                           // обязательный завершающий элемент
};
MODULE_DEVICE_TABLE(usb, skel_table);         // регистрирует таблицу в USB-core → udev и hotplug


// Базовый номер minor для наших устройств (/dev/skel0, /dev/skel1 и т.д.)
#define USB_SKEL_MINOR_BASE   192


// Ограничение на размер одного трансфера (чтобы не перегружать VM и не выходить за страницу)
#define MAX_TRANSFER          (PAGE_SIZE - 512)
// 512 — максимальный размер пакета в EHCI, оставляем запас

#define WRITES_IN_FLIGHT      8       // Сколько одновременных операций записи разрешаем (защита от исчерпания памяти)


// Главная структура, которая хранит всё состояние нашего устройства
struct usb_skel {
    struct usb_device       *udev;              // указатель на USB-устройство
    struct usb_interface    *interface;         // текущий интерфейс USB-устройства

    struct semaphore        limit_sem;          // семафор — ограничивает кол-во одновременных write
    struct usb_anchor       submitted;          // якорь для всех поданных URB (удобно убивать все разом)

    struct urb              *bulk_in_urb;       // URB для чтения по bulk-in
    unsigned char           *bulk_in_buffer;    // буфер приёма данных (обычно выделяется kmalloc)
    size_t                  bulk_in_size;       // размер этого буфера
    size_t                  bulk_in_filled;     // сколько байт реально пришло в буфер
    size_t                  bulk_in_copied;     // сколько байт уже отдано пользователю

    __u8                    bulk_in_endpointAddr;   // адрес endpoint для чтения (Bulk IN)
    __u8                    bulk_out_endpointAddr;  // адрес endpoint для записи (Bulk OUT)

    int                     errors;             // последняя ошибка (храним для передачи в read/write)
    bool                    ongoing_read;       // флаг — выполняется ли сейчас чтение

    spinlock_t              err_lock;           // спинлок для защиты поля errors и ongoing_read
    struct kref             kref;               // счётчик ссылок — когда станет 0 → удаляем структуру
    struct mutex            io_mutex;           // большой мьютекс — защищает все операции ввода-вывода
    unsigned long           disconnected:1;     // флаг — устройство уже отключено

    wait_queue_head_t       bulk_in_wait;       // очередь ожидания завершения операции чтения
};

#define to_skel_dev(d) container_of(d, struct usb_skel, kref)
// удобный макрос для получения указателя на нашу структуру из kref


// Функция, которая вызывается, когда kref достигает нуля
static void skel_delete(struct kref *kref)
{
    struct usb_skel *dev = to_skel_dev(kref);

    usb_free_urb(dev->bulk_in_urb);             // освобождаем URB чтения
    usb_put_intf(dev->interface);               // уменьшаем счётчик ссылок на интерфейс
    usb_put_dev(dev->udev);                     // уменьшаем счётчик ссылок на usb_device
    kfree(dev->bulk_in_buffer);                 // освобождаем буфер приёма
    kfree(dev);                                 // освобождаем саму структуру устройства
}


// Открытие файла /dev/skelX
static int skel_open(struct inode *inode, struct file *file)
{
    struct usb_skel *dev;
    struct usb_interface *interface;
    int subminor;
    int retval = 0;

    subminor = iminor(inode);                   // получаем minor-номер нашего устройства

    // Находим интерфейс по minor-номеру
    interface = usb_find_interface(&skel_driver, subminor);
    if (!interface) {
        pr_err("%s - error, can't find device for minor %d\n", __func__, subminor);
        retval = -ENODEV;
        goto exit;
    }

    dev = usb_get_intfdata(interface);          // получаем нашу структуру из интерфейса
    if (!dev) {
        retval = -ENODEV;
        goto exit;
    }

    // Предотвращаем autosuspend пока устройство открыто
    retval = usb_autopm_get_interface(interface);
    if (retval)
        goto exit;

    kref_get(&dev->kref);                       // увеличиваем счётчик ссылок

    file->private_data = dev;                   // сохраняем указатель для всех последующих операций

exit:
    return retval;
}


// Закрытие файла /dev/skelX
static int skel_release(struct inode *inode, struct file *file)
{
    struct usb_skel *dev = file->private_data;

    if (!dev)
        return -ENODEV;

    // Разрешаем устройству снова уходить в autosuspend
    usb_autopm_put_interface(dev->interface);

    // Уменьшаем счётчик ссылок → если 0, то вызовется skel_delete
    kref_put(&dev->kref, skel_delete);

    return 0;
}


// Вызывается при close() или когда нужно дождаться завершения операций
static int skel_flush(struct file *file, fl_owner_t id)
{
    struct usb_skel *dev = file->private_data;
    int res;

    if (!dev)
        return -ENODEV;

    mutex_lock(&dev->io_mutex);
    skel_draw_down(dev);                        // останавливаем все активные URB

    // Считываем и сбрасываем последнюю ошибку
    spin_lock_irq(&dev->err_lock);
    res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
    dev->errors = 0;
    spin_unlock_irq(&dev->err_lock);

    mutex_unlock(&dev->io_mutex);

    return res;
}


// Callback — вызывается, когда URB чтения завершился
static void skel_read_bulk_callback(struct urb *urb)
{
    struct usb_skel *dev = urb->context;
    unsigned long flags;

    spin_lock_irqsave(&dev->err_lock, flags);

    if (urb->status) {
        // нормальные ошибки при отключении/сбросе не считаем критичными
        if (!(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN))
            dev_err(&dev->interface->dev, "%s - nonzero read status: %d\n", __func__, urb->status);

        dev->errors = urb->status;
    } else {
        dev->bulk_in_filled = urb->actual_length;   // сколько реально пришло данных
    }

    dev->ongoing_read = 0;                          // чтение завершено

    spin_unlock_irqrestore(&dev->err_lock, flags);

    wake_up_interruptible(&dev->bulk_in_wait);      // будим тех, кто ждёт в read()
}


// Вспомогательная функция — запускает чтение по bulk-in
static int skel_do_read_io(struct usb_skel *dev, size_t count)
{
    int rv;

    // Настраиваем URB для bulk-чтения
    usb_fill_bulk_urb(dev->bulk_in_urb,
                      dev->udev,
                      usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
                      dev->bulk_in_buffer,
                      min(dev->bulk_in_size, count),
                      skel_read_bulk_callback,
                      dev);

    spin_lock_irq(&dev->err_lock);
    dev->ongoing_read = 1;
    spin_unlock_irq(&dev->err_lock);

    dev->bulk_in_filled = 0;
    dev->bulk_in_copied = 0;

    rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
    if (rv < 0) {
        dev_err(&dev->interface->dev, "%s - submit read urb failed: %d\n", __func__, rv);
        rv = (rv == -ENOMEM) ? rv : -EIO;
        spin_lock_irq(&dev->err_lock);
        dev->ongoing_read = 0;
        spin_unlock_irq(&dev->err_lock);
    }

    return rv;
}


// Чтение из /dev/skelX
static ssize_t skel_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
    struct usb_skel *dev = file->private_data;
    int rv;
    bool ongoing_io;

    if (!count)
        return 0;

    rv = mutex_lock_interruptible(&dev->io_mutex);
    if (rv < 0)
        return rv;

    if (dev->disconnected) {
        rv = -ENODEV;
        goto exit;
    }

retry:
    spin_lock_irq(&dev->err_lock);
    ongoing_io = dev->ongoing_read;
    spin_unlock_irq(&dev->err_lock);

    // Если чтение уже идёт — ждём (кроме O_NONBLOCK)
    if (ongoing_io) {
        if (file->f_flags & O_NONBLOCK) {
            rv = -EAGAIN;
            goto exit;
        }
        rv = wait_event_interruptible(dev->bulk_in_wait, (!dev->ongoing_read));
        if (rv < 0)
            goto exit;
    }

    // Проверяем, была ли ошибка
    rv = dev->errors;
    if (rv < 0) {
        dev->errors = 0;
        rv = (rv == -EPIPE) ? rv : -EIO;
        goto exit;
    }

    // Есть данные в буфере — отдаём пользователю
    if (dev->bulk_in_filled) {
        size_t available = dev->bulk_in_filled - dev->bulk_in_copied;
        size_t chunk = min(available, count);

        if (!available) {
            // буфер исчерпан — запускаем новое чтение
            rv = skel_do_read_io(dev, count);
            if (rv < 0)
                goto exit;
            else
                goto retry;
        }

        if (copy_to_user(buffer, dev->bulk_in_buffer + dev->bulk_in_copied, chunk))
            rv = -EFAULT;
        else
            rv = chunk;

        dev->bulk_in_copied += chunk;

        // если просили больше — начинаем следующее чтение асинхронно
        if (available < count)
            skel_do_read_io(dev, count - chunk);
    } else {
        // данных нет — запускаем чтение
        rv = skel_do_read_io(dev, count);
        if (rv < 0)
            goto exit;
        else
            goto retry;
    }

exit:
    mutex_unlock(&dev->io_mutex);
    return rv;
}


// Callback для завершённой операции записи
static void skel_write_bulk_callback(struct urb *urb)
{
    struct usb_skel *dev = urb->context;
    unsigned long flags;

    if (urb->status &&                                      // ошибка
        !(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)) {
        dev_err(&dev->interface->dev, "%s - write status: %d\n", __func__, urb->status);
    }

    // Освобождаем coherent-буфер, который использовался для DMA
    usb_free_coherent(urb->dev, urb->transfer_buffer_length,
                      urb->transfer_buffer, urb->transfer_dma);

    up(&dev->limit_sem);                                    // разрешаем следующую запись
}


// Запись в /dev/skelX
static ssize_t skel_write(struct file *file, const char __user *user_buffer,
                          size_t count, loff_t *ppos)
{
    struct usb_skel *dev = file->private_data;
    int retval = 0;
    struct urb *urb = NULL;
    char *buf = NULL;
    size_t writesize = min_t(size_t, count, MAX_TRANSFER);

    if (count == 0)
        goto exit;

    // Ограничиваем количество одновременных write (защита от OOM)
    if (!(file->f_flags & O_NONBLOCK)) {
        if (down_interruptible(&dev->limit_sem)) {
            retval = -ERESTARTSYS;
            goto exit;
        }
    } else {
        if (down_trylock(&dev->limit_sem)) {
            retval = -EAGAIN;
            goto exit;
        }
    }

    // Проверяем, нет ли уже ошибки
    spin_lock_irq(&dev->err_lock);
    retval = dev->errors;
    if (retval < 0) {
        dev->errors = 0;
        retval = (retval == -EPIPE) ? retval : -EIO;
    }
    spin_unlock_irq(&dev->err_lock);
    if (retval < 0)
        goto error;

    urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!urb) {
        retval = -ENOMEM;
        goto error;
    }

    // Выделяем память, пригодную для DMA (coherent)
    buf = usb_alloc_coherent(dev->udev, writesize, GFP_KERNEL, &urb->transfer_dma);
    if (!buf) {
        retval = -ENOMEM;
        goto error;
    }

    if (copy_from_user(buf, user_buffer, writesize)) {
        retval = -EFAULT;
        goto error;
    }

    mutex_lock(&dev->io_mutex);
    if (dev->disconnected) {
        mutex_unlock(&dev->io_mutex);
        retval = -ENODEV;
        goto error;
    }

    // Настраиваем URB для bulk-записи
    usb_fill_bulk_urb(urb, dev->udev,
                      usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
                      buf, writesize, skel_write_bulk_callback, dev);

    urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;     // мы уже указали DMA-адрес

    usb_anchor_urb(urb, &dev->submitted);               // добавляем в список активных URB

    retval = usb_submit_urb(urb, GFP_KERNEL);
    mutex_unlock(&dev->io_mutex);

    if (retval) {
        dev_err(&dev->interface->dev, "%s - submit write urb failed: %d\n", __func__, retval);
        goto error_unanchor;
    }

    usb_free_urb(urb);              // уменьшаем счётчик — URB освободится после callback

    return writesize;

error_unanchor:
    usb_unanchor_urb(urb);
error:
    if (urb) {
        usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
        usb_free_urb(urb);
    }
    up(&dev->limit_sem);

exit:
    return retval;
}


// Операции файла для нашего символьного устройства
static const struct file_operations skel_fops = {
    .owner   = THIS_MODULE,
    .read    = skel_read,
    .write   = skel_write,
    .open    = skel_open,
    .release = skel_release,
    .flush   = skel_flush,
    .llseek  = noop_llseek,         // не поддерживаем смещение
};


// Информация о классе устройства — нужна для получения minor и регистрации в sysfs
static struct usb_class_driver skel_class = {
    .name        = "skel%d",                // шаблон имени → /dev/skel0, skel1...
    .fops        = &skel_fops,
    .minor_base  = USB_SKEL_MINOR_BASE,
};


// Функция probe — вызывается при подключении подходящего устройства
static int skel_probe(struct usb_interface *interface,
                      const struct usb_device_id *id)
{
    struct usb_skel *dev = NULL;
    struct usb_endpoint_descriptor *bulk_in = NULL, *bulk_out = NULL;
    int retval = -ENOMEM;

    // Выделяем память под состояние устройства
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    kref_init(&dev->kref);                      // инициализируем счётчик ссылок = 1
    sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
    mutex_init(&dev->io_mutex);
    spin_lock_init(&dev->err_lock);
    init_usb_anchor(&dev->submitted);
    init_waitqueue_head(&dev->bulk_in_wait);

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = usb_get_intf(interface);

    // Ищем первый Bulk IN и первый Bulk OUT endpoint
    retval = usb_find_common_endpoints(interface->cur_altsetting,
                                       &bulk_in, &bulk_out, NULL, NULL);
    if (retval) {
        dev_err(&interface->dev, "Не найдены bulk-in и bulk-out endpoints\n");
        goto error;
    }

    dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
    dev->bulk_in_endpointAddr = bulk_in->bEndpointAddress;
    dev->bulk_out_endpointAddr = bulk_out->bEndpointAddress;

    // Выделяем буфер для приёма
    dev->bulk_in_buffer = kmalloc(dev->bulk_in_size, GFP_KERNEL);
    if (!dev->bulk_in_buffer)
        goto error;

    // Выделяем URB для чтения (он будет переиспользоваться)
    dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->bulk_in_urb)
        goto error;

    usb_set_intfdata(interface, dev);           // привязываем нашу структуру к интерфейсу

    // Регистрируем символьное устройство → появляется /dev/skelX
    retval = usb_register_dev(interface, &skel_class);
    if (retval) {
        dev_err(&interface->dev, "Не удалось получить minor-номер\n");
        usb_set_intfdata(interface, NULL);
        goto error;
    }

    dev_info(&interface->dev, "USB Skeleton device attached to USBSkel-%d\n",
             interface->minor);

    return 0;

error:
    kref_put(&dev->kref, skel_delete);          // при ошибке уменьшаем счётчик → удаление
    return retval;
}


// Вызывается при отключении устройства
static void skel_disconnect(struct usb_interface *interface)
{
    struct usb_skel *dev;
    int minor = interface->minor;

    dev = usb_get_intfdata(interface);

    usb_deregister_dev(interface, &skel_class);     // убираем /dev/skelX

    mutex_lock(&dev->io_mutex);
    dev->disconnected = 1;
    mutex_unlock(&dev->io_mutex);

    // Убиваем все активные URB
    usb_kill_urb(dev->bulk_in_urb);
    usb_kill_anchored_urbs(&dev->submitted);

    kref_put(&dev->kref, skel_delete);              // уменьшаем счётчик → удаление при 0

    dev_info(&interface->dev, "USB Skeleton #%d disconnected\n", minor);
}


// Вспомогательная функция — останавливает все операции перед suspend/reset
static void skel_draw_down(struct usb_skel *dev)
{
    int time;

    // Ждём максимум 1 секунду, пока все write завершатся
    time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
    if (!time)
        usb_kill_anchored_urbs(&dev->submitted);

    usb_kill_urb(dev->bulk_in_urb);
}


// Power management — suspend
static int skel_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct usb_skel *dev = usb_get_intfdata(intf);
    if (!dev)
        return 0;
    skel_draw_down(dev);
    return 0;
}

// Power management — resume
static int skel_resume(struct usb_interface *intf)
{
    return 0;   // в шаблоне ничего не возобновляем — можно добавить логику
}

// Вызывается перед USB reset
static int skel_pre_reset(struct usb_interface *intf)
{
    struct usb_skel *dev = usb_get_intfdata(intf);
    mutex_lock(&dev->io_mutex);
    skel_draw_down(dev);
    return 0;
}

// Вызывается после USB reset
static int skel_post_reset(struct usb_interface *intf)
{
    struct usb_skel *dev = usb_get_intfdata(intf);
    dev->errors = -EPIPE;           // сигнализируем, что была ошибка (pipe reset)
    mutex_unlock(&dev->io_mutex);
    return 0;
}


// Главная структура драйвера — регистрируется в USB-core
static struct usb_driver skel_driver = {
    .name =           "skeleton",
    .probe =          skel_probe,
    .disconnect =     skel_disconnect,
    .suspend =        skel_suspend,
    .resume =         skel_resume,
    .pre_reset =      skel_pre_reset,
    .post_reset =     skel_post_reset,
    .id_table =       skel_table,
    .supports_autosuspend = 1,      // разрешаем autosuspend
};

module_usb_driver(skel_driver);         // удобный макрос — заменяет module_init + регистрацию

MODULE_LICENSE("GPL v2");
