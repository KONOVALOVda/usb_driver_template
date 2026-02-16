// Это строка говорит, что код распространяется по лицензии GPL версии 2
// Без неё код нельзя включить в официальное ядро Linux
SPDX-License-Identifier: GPL-2.0

// Это многострочный комментарий — просто описание файла
/*
 * USB Skeleton driver - 2.2
 * Это пример (шаблон) драйвера для USB-устройств
 * Его написал Greg Kroah-Hartman много лет назад
 * А потом много раз улучшали
 */

#include <linux/kernel.h>      // Здесь лежат самые основные вещи ядра: printk, KERN_INFO и т.д.
#include <linux/errno.h>       // Здесь находятся коды ошибок: -ENOMEM, -ENODEV, -EIO …
#include <linux/slab.h>        // Функции для выделения памяти в ядре: kzalloc, kfree
#include <linux/module.h>      // Очень важный заголовок: module_init, MODULE_LICENSE, THIS_MODULE
#include <linux/kref.h>        // Счётчик ссылок — помогает понять, когда можно безопасно удалить объект
#include <linux/uaccess.h>     // copy_to_user и copy_from_user — копируем данные между ядром и программами
#include <linux/usb.h>         // Самый главный заголовок для работы с USB в ядре
#include <linux/mutex.h>       // Мьютексы — чтобы несколько частей кода не мешали друг другу


// Эти два числа — как паспорт устройства
// Каждое USB-устройство имеет такие два идентификатора
#define USB_SKEL_VENDOR_ID    0xfff0     // Номер производителя (Vendor ID)
#define USB_SKEL_PRODUCT_ID   0xfff0     // Номер конкретной модели (Product ID)

// Эта таблица говорит ядру: «если увидишь устройство с этими номерами — позови наш драйвер»
static const struct usb_device_id skel_table[] = {
    { USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },   // одно устройство
    { }                                                        // пустая строка в конце — обязательно!
};

// Эта строка делает таблицу видимой для системы — чтобы она знала, какие устройства мы поддерживаем
MODULE_DEVICE_TABLE(usb, skel_table);


// С какого номера начинать давать имена нашим устройствам (/dev/skel0, /dev/skel1 …)
#define USB_SKEL_MINOR_BASE   192


// Самый большой кусок данных, который мы будем передавать за один раз
// Берём почти размер страницы памяти, но чуть меньше
#define MAX_TRANSFER          (PAGE_SIZE - 512)

// Сколько операций записи мы разрешаем делать одновременно
// Больше восьми — уже опасно, может закончиться память
#define WRITES_IN_FLIGHT      8


// Это самая важная структура — как «паспорт» нашего устройства
// В ней мы храним всё, что нам может понадобиться
struct usb_skel {
    struct usb_device       *udev;              // указатель на само USB-устройство
    struct usb_interface    *interface;         // указатель на интерфейс (часть устройства)

    struct semaphore        limit_sem;          // счётчик, сколько ещё можно писать
    struct usb_anchor       submitted;          // список всех отправленных запросов (чтобы можно было их отменить)

    struct urb              *bulk_in_urb;       // запрос на чтение данных
    unsigned char           *bulk_in_buffer;    // место, куда придут данные от устройства
    size_t                  bulk_in_size;       // сколько байт в этом буфере
    size_t                  bulk_in_filled;     // сколько байт уже пришло
    size_t                  bulk_in_copied;     // сколько из них мы уже отдали программе

    __u8                    bulk_in_endpointAddr;   // номер «трубки», по которой приходят данные
    __u8                    bulk_out_endpointAddr;  // номер «трубки», по которой мы отправляем данные

    int                     errors;             // последняя ошибка, которую мы запомнили
    bool                    ongoing_read;       // true = сейчас идёт чтение

    spinlock_t              err_lock;           // замочек для безопасного изменения errors и ongoing_read
    struct kref             kref;               // счётчик «сколько людей пользуются этим устройством»
    struct mutex            io_mutex;           // большой замок — защищает почти все операции
    unsigned long           disconnected:1;     // флажок: устройство уже вытащили

    wait_queue_head_t       bulk_in_wait;       // очередь — здесь ждут, пока придут данные
};

// Удобный способ быстро получить нашу структуру из kref
#define to_skel_dev(d) container_of(d, struct usb_skel, kref)


// Эта функция вызывается, когда никто больше не пользуется устройством
static void skel_delete(struct kref *kref)
{
    struct usb_skel *dev = to_skel_dev(kref);   // получаем нашу структуру

    usb_free_urb(dev->bulk_in_urb);             // освобождаем запрос на чтение
    usb_put_intf(dev->interface);               // говорим: «мы больше не держим интерфейс»
    usb_put_dev(dev->udev);                     // говорим: «мы больше не держим устройство»
    kfree(dev->bulk_in_buffer);                 // освобождаем память под буфер
    kfree(dev);                                 // наконец освобождаем саму структуру
}


// Когда программа открывает файл /dev/skel0
static int skel_open(struct inode *inode, struct file *file)
{
    struct usb_skel *dev;
    struct usb_interface *interface;
    int subminor;
    int retval = 0;

    subminor = iminor(inode);                   // узнаём номер нашего устройства (0,1,2…)

    // ищем, к какому USB-интерфейсу относится этот номер
    interface = usb_find_interface(&skel_driver, subminor);
    if (!interface) {
        pr_err("%s - не нашли устройство с номером %d\n", __func__, subminor);
        retval = -ENODEV;
        goto exit;
    }

    dev = usb_get_intfdata(interface);          // берём нашу структуру, которую мы туда положили
    if (!dev) {
        retval = -ENODEV;
        goto exit;
    }

    // говорим USB: «пока это устройство открыто — не выключай его»
    retval = usb_autopm_get_interface(interface);
    if (retval)
        goto exit;

    kref_get(&dev->kref);                       // +1 к счётчику пользователей

    file->private_data = dev;                   // запоминаем для всех остальных функций

exit:
    return retval;
}


// Когда программа закрывает файл /dev/skelX
static int skel_release(struct inode *inode, struct file *file)
{
    struct usb_skel *dev = file->private_data;

    if (!dev)
        return -ENODEV;

    // теперь можно снова разрешить устройству «засыпать»
    usb_autopm_put_interface(dev->interface);

    // -1 к счётчику пользователей
    // если стало 0 → вызовется skel_delete
    kref_put(&dev->kref, skel_delete);

    return 0;
}


// Эта функция вызывается, когда программа закрывает файл
// или хочет убедиться, что все операции закончились
static int skel_flush(struct file *file, fl_owner_t id)
{
    struct usb_skel *dev = file->private_data;
    int res;

    if (!dev)
        return -ENODEV;

    mutex_lock(&dev->io_mutex);                 // берём большой замок
    skel_draw_down(dev);                        // останавливаем все запросы

    // смотрим, была ли ошибка
    spin_lock_irq(&dev->err_lock);
    res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
    dev->errors = 0;                            // сбрасываем ошибку
    spin_unlock_irq(&dev->err_lock);

    mutex_unlock(&dev->io_mutex);

    return res;
}


// Эта функция автоматически вызывается, когда пришёл ответ на запрос чтения
static void skel_read_bulk_callback(struct urb *urb)
{
    struct usb_skel *dev = urb->context;
    unsigned long flags;

    spin_lock_irqsave(&dev->err_lock, flags);

    if (urb->status) {                          // если была ошибка
        // некоторые ошибки нормальные (устройство отключили и т.д.)
        if (!(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN))
            dev_err(&dev->interface->dev, "ошибка чтения: %d\n", urb->status);

        dev->errors = urb->status;
    } else {
        dev->bulk_in_filled = urb->actual_length;   // сколько байт реально пришло
    }

    dev->ongoing_read = 0;                      // чтение закончилось

    spin_unlock_irqrestore(&dev->err_lock, flags);

    // будим всех, кто ждал в функции read()
    wake_up_interruptible(&dev->bulk_in_wait);
}


// Маленькая функция-помощник: запускает чтение из USB
static int skel_do_read_io(struct usb_skel *dev, size_t count)
{
    int rv;

    // готовим запрос на чтение
    usb_fill_bulk_urb(dev->bulk_in_urb,
                      dev->udev,
                      usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
                      dev->bulk_in_buffer,
                      min(dev->bulk_in_size, count),
                      skel_read_bulk_callback,
                      dev);

    spin_lock_irq(&dev->err_lock);
    dev->ongoing_read = 1;                      // ставим флажок «чтение идёт»
    spin_unlock_irq(&dev->err_lock);

    dev->bulk_in_filled = 0;
    dev->bulk_in_copied = 0;

    // отправляем запрос в USB
    rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
    if (rv < 0) {
        dev_err(&dev->interface->dev, "не удалось отправить запрос на чтение: %d\n", rv);
        rv = (rv == -ENOMEM) ? rv : -EIO;
        spin_lock_irq(&dev->err_lock);
        dev->ongoing_read = 0;
        spin_unlock_irq(&dev->err_lock);
    }

    return rv;
}
// Чтение из файла /dev/skelX (программа хочет получить данные от USB-устройства)
static ssize_t skel_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
    struct usb_skel *dev = file->private_data;      // берём нашу структуру устройства
    int rv;                                         // сюда будем класть результат (сколько байт отдали или ошибку)
    bool ongoing_io;                                // флажок: идёт ли сейчас чтение

    if (!count)                                     // если программа попросила 0 байт — сразу выходим
        return 0;

    // берём большой замок — чтобы никто другой не мешал
    rv = mutex_lock_interruptible(&dev->io_mutex);
    if (rv < 0)                                     // если нас прервали сигналом — возвращаем ошибку
        return rv;

    if (dev->disconnected) {                        // если устройство уже вытащили
        rv = -ENODEV;                               // говорим: «устройства нет»
        goto exit;
    }

retry:                                              // метка — сюда будем возвращаться, если нужно подождать

    // проверяем, не идёт ли уже чтение
    spin_lock_irq(&dev->err_lock);
    ongoing_io = dev->ongoing_read;
    spin_unlock_irq(&dev->err_lock);

    if (ongoing_io) {                               // если чтение уже запущено
        if (file->f_flags & O_NONBLOCK) {           // если программа сказала «не ждать»
            rv = -EAGAIN;                           // говорим: «занято, попробуй позже»
            goto exit;
        }

        // ждём, пока чтение закончится (можно прервать сигналом)
        rv = wait_event_interruptible(dev->bulk_in_wait, (!dev->ongoing_read));
        if (rv < 0)                                 // если нас разбудили сигналом
            goto exit;
    }

    // смотрим, была ли ошибка при прошлом чтении
    rv = dev->errors;
    if (rv < 0) {
        dev->errors = 0;                            // сбрасываем ошибку (чтобы не повторять)
        rv = (rv == -EPIPE) ? rv : -EIO;            // сохраняем особую ошибку pipe reset
        goto exit;
    }

    // если в буфере уже есть данные
    if (dev->bulk_in_filled) {
        size_t available = dev->bulk_in_filled - dev->bulk_in_copied;  // сколько осталось
        size_t chunk = min(available, count);                          // сколько можем отдать сейчас

        if (!available) {                           // если всё уже отдали
            rv = skel_do_read_io(dev, count);       // запускаем новое чтение
            if (rv < 0)
                goto exit;
            else
                goto retry;                         // и ждём его завершения
        }

        // копируем данные из ядра в программу пользователя
        if (copy_to_user(buffer, dev->bulk_in_buffer + dev->bulk_in_copied, chunk))
            rv = -EFAULT;                           // ошибка копирования
        else
            rv = chunk;                             // сколько байт успешно отдали

        dev->bulk_in_copied += chunk;               // сдвигаем указатель

        // если программа хочет ещё больше — начинаем читать дальше (не ждём)
        if (available < count)
            skel_do_read_io(dev, count - chunk);
    } else {
        // данных в буфере нет — запускаем чтение
        rv = skel_do_read_io(dev, count);
        if (rv < 0)
            goto exit;
        else
            goto retry;                             // ждём, пока придут данные
    }

exit:
    mutex_unlock(&dev->io_mutex);                   // отпускаем большой замок
    return rv;                                      // возвращаем количество байт или ошибку
}


// Эта функция вызывается, когда USB закончил отправлять наши данные
static void skel_write_bulk_callback(struct urb *urb)
{
    struct usb_skel *dev = urb->context;
    unsigned long flags;

    // если была ошибка (и это не нормальная ошибка отключения)
    if (urb->status &&
        !(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)) {
        dev_err(&dev->interface->dev, "ошибка записи: %d\n", urb->status);
    }

    // освобождаем память, которую мы выделяли специально для USB (DMA-память)
    usb_free_coherent(urb->dev, urb->transfer_buffer_length,
                      urb->transfer_buffer, urb->transfer_dma);

    up(&dev->limit_sem);                            // разрешаем следующую запись (счётчик +1)
}


// Запись в файл /dev/skelX (программа хочет отправить данные на USB-устройство)
static ssize_t skel_write(struct file *file, const char __user *user_buffer,
                          size_t count, loff_t *ppos)
{
    struct usb_skel *dev = file->private_data;
    int retval = 0;
    struct urb *urb = NULL;
    char *buf = NULL;
    size_t writesize = min_t(size_t, count, MAX_TRANSFER);  // не больше, чем мы разрешаем

    if (count == 0)                                 // ничего не пишут — сразу выходим
        goto exit;

    // проверяем, сколько ещё можно писать одновременно
    if (!(file->f_flags & O_NONBLOCK)) {            // если программа готова ждать
        if (down_interruptible(&dev->limit_sem)) {  // ждём место (можно прервать сигналом)
            retval = -ERESTARTSYS;
            goto exit;
        }
    } else {                                        // если не хочет ждать
        if (down_trylock(&dev->limit_sem)) {        // если места нет — сразу ошибка
            retval = -EAGAIN;
            goto exit;
        }
    }

    // смотрим, нет ли уже старой ошибки
    spin_lock_irq(&dev->err_lock);
    retval = dev->errors;
    if (retval < 0) {
        dev->errors = 0;
        retval = (retval == -EPIPE) ? retval : -EIO;
    }
    spin_unlock_irq(&dev->err_lock);
    if (retval < 0)
        goto error;

    // создаём новый запрос USB
    urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!urb) {
        retval = -ENOMEM;
        goto error;
    }

    // выделяем специальную память для USB (DMA-совместимую)
    buf = usb_alloc_coherent(dev->udev, writesize, GFP_KERNEL, &urb->transfer_dma);
    if (!buf) {
        retval = -ENOMEM;
        goto error;
    }

    // копируем данные из программы пользователя в наш буфер
    if (copy_from_user(buf, user_buffer, writesize)) {
        retval = -EFAULT;
        goto error;
    }

    mutex_lock(&dev->io_mutex);
    if (dev->disconnected) {                        // если устройство уже вытащили
        mutex_unlock(&dev->io_mutex);
        retval = -ENODEV;
        goto error;
    }

    // настраиваем запрос на отправку
    usb_fill_bulk_urb(urb, dev->udev,
                      usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
                      buf, writesize, skel_write_bulk_callback, dev);

    urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP; // говорим: «я уже указал адрес DMA»

    usb_anchor_urb(urb, &dev->submitted);           // добавляем в список активных запросов

    // отправляем данные в устройство
    retval = usb_submit_urb(urb, GFP_KERNEL);
    mutex_unlock(&dev->io_mutex);

    if (retval) {
        dev_err(&dev->interface->dev, "не удалось отправить данные: %d\n", retval);
        goto error_unanchor;
    }

    usb_free_urb(urb);                              // уменьшаем счётчик — URB сам освободится позже

    return writesize;                               // говорим, сколько байт приняли

error_unanchor:
    usb_unanchor_urb(urb);                          // убираем из списка (если не отправили)
error:
    if (urb) {
        usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
        usb_free_urb(urb);
    }
    up(&dev->limit_sem);                            // возвращаем место в очереди записи

exit:
    return retval;
}


// Это таблица операций, которые можно делать с нашим файлом /dev/skelX
static const struct file_operations skel_fops = {
    .owner   = THIS_MODULE,                         // это наш модуль владеет этими функциями
    .read    = skel_read,                           // функция чтения
    .write   = skel_write,                          // функция записи
    .open    = skel_open,                           // открыть устройство
    .release = skel_release,                        // закрыть устройство
    .flush   = skel_flush,                          // дождаться завершения операций
    .llseek  = noop_llseek,                         // не умеем перемещаться по файлу
};
// Это описание нашего "класса" устройства
// Благодаря этому ядро создаст файл /dev/skel0, /dev/skel1 и т.д.
static struct usb_class_driver skel_class = {
    .name        = "skel%d",                    // %d — это номер (0,1,2...), получится skel0, skel1...
    .fops        = &skel_fops,                  // указываем таблицу функций (open, read, write...)
    .minor_base  = USB_SKEL_MINOR_BASE,         // с какого номера начинать (192)
};


// Самая главная функция — probe
// Вызывается автоматически, когда мы подключили подходящее USB-устройство
static int skel_probe(struct usb_interface *interface,
                      const struct usb_device_id *id)
{
    struct usb_skel *dev = NULL;                // наша будущая структура
    struct usb_endpoint_descriptor *bulk_in = NULL;   // описание "трубки" для чтения
    struct usb_endpoint_descriptor *bulk_out = NULL;  // описание "трубки" для записи
    int retval = -ENOMEM;                       // по умолчанию — ошибка "не хватило памяти"

    // Выделяем память под всю нашу структуру устройства
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)                                   // если памяти не хватило
        return -ENOMEM;

    kref_init(&dev->kref);                      // ставим счётчик пользователей = 1

    sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);   // инициализируем счётчик одновременных записей
    mutex_init(&dev->io_mutex);                 // создаём большой замок
    spin_lock_init(&dev->err_lock);             // создаём маленький быстрый замок для ошибок
    init_usb_anchor(&dev->submitted);           // готовим "якорь" для всех отправленных запросов
    init_waitqueue_head(&dev->bulk_in_wait);    // создаём очередь ожидания для чтения

    // Сохраняем указатели на устройство и интерфейс (чтобы потом не потерять)
    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = usb_get_intf(interface);

    // Ищем в устройстве первую трубку для чтения и первую для записи
    retval = usb_find_common_endpoints(interface->cur_altsetting,
                                       &bulk_in, &bulk_out, NULL, NULL);
    if (retval) {                               // если не нашли обе трубки
        dev_err(&interface->dev, "Не нашли трубки для чтения и записи!\n");
        goto error;
    }

    // Запоминаем размер буфера чтения (обычно 512, 1024 или больше)
    dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
    dev->bulk_in_endpointAddr = bulk_in->bEndpointAddress;    // номер трубки чтения
    dev->bulk_out_endpointAddr = bulk_out->bEndpointAddress;  // номер трубки записи

    // Выделяем память под буфер, куда будут приходить данные от USB
    dev->bulk_in_buffer = kmalloc(dev->bulk_in_size, GFP_KERNEL);
    if (!dev->bulk_in_buffer) {
        retval = -ENOMEM;
        goto error;
    }

    // Создаём один запрос на чтение (потом будем его много раз использовать)
    dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->bulk_in_urb) {
        retval = -ENOMEM;
        goto error;
    }

    // Привязываем нашу структуру к интерфейсу USB
    // Теперь ядро знает: "это устройство принадлежит нашему драйверу"
    usb_set_intfdata(interface, dev);

    // Регистрируем символьное устройство → появляется файл /dev/skelX
    retval = usb_register_dev(interface, &skel_class);
    if (retval) {                               // если не получилось (например, все номера заняты)
        dev_err(&interface->dev, "Не удалось создать файл /dev/skelX\n");
        usb_set_intfdata(interface, NULL);
        goto error;
    }

    // Говорим в лог: "Ура! Устройство подключилось!"
    dev_info(&interface->dev, "USB Skeleton device attached to USBSkel-%d\n",
             interface->minor);

    return 0;                                   // успех! Всё готово

error:                                          // если где-то ошибка — чистим за собой
    kref_put(&dev->kref, skel_delete);          // уменьшаем счётчик → вызовется удаление
    return retval;
}


// Функция вызывается, когда устройство отключили (вытащили из USB-порта)
static void skel_disconnect(struct usb_interface *interface)
{
    struct usb_skel *dev;
    int minor = interface->minor;               // запоминаем номер устройства

    dev = usb_get_intfdata(interface);          // берём нашу структуру

    // Убираем файл /dev/skelX из системы
    usb_deregister_dev(interface, &skel_class);

    // Берём замок, чтобы никто не начал новые операции
    mutex_lock(&dev->io_mutex);
    dev->disconnected = 1;                      // ставим флажок "устройства больше нет"
    mutex_unlock(&dev->io_mutex);

    // Убиваем все запросы, которые могли висеть
    usb_kill_urb(dev->bulk_in_urb);
    usb_kill_anchored_urbs(&dev->submitted);

    // Уменьшаем счётчик пользователей → если никто не держит — удаляем структуру
    kref_put(&dev->kref, skel_delete);

    // Пишем в лог: "устройство отключено"
    dev_info(&interface->dev, "USB Skeleton #%d disconnected\n", minor);
}


// Вспомогательная функция: останавливает все текущие операции
// Используется перед "сном", сбросом и отключением
static void skel_draw_down(struct usb_skel *dev)
{
    int time;

    // Ждём максимум 1 секунду, пока все записи закончатся
    time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
    if (!time)                                  // если не дождались
        usb_kill_anchored_urbs(&dev->submitted);   // принудительно убиваем все запросы

    usb_kill_urb(dev->bulk_in_urb);             // убиваем запрос на чтение
}


// Когда устройство "засыпает" (suspend) — например, ноутбук ушёл в спящий режим
static int skel_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct usb_skel *dev = usb_get_intfdata(intf);
    if (!dev)
        return 0;
    skel_draw_down(dev);                        // останавливаем все операции
    return 0;
}


// Когда устройство "просыпается" (resume)
static int skel_resume(struct usb_interface *intf)
{
    return 0;                                   // в этом примере ничего не делаем
    // Можно добавить здесь повторную инициализацию, если нужно
}


// Перед тем, как USB сделает сброс (reset) устройства
static int skel_pre_reset(struct usb_interface *intf)
{
    struct usb_skel *dev = usb_get_intfdata(intf);
    mutex_lock(&dev->io_mutex);                 // берём замок
    skel_draw_down(dev);                        // останавливаем всё
    return 0;
}


// После того, как USB сбросил устройство
static int skel_post_reset(struct usb_interface *intf)
{
    struct usb_skel *dev = usb_get_intfdata(intf);

    dev->errors = -EPIPE;                       // запоминаем, что была ошибка (pipe reset)

    mutex_unlock(&dev->io_mutex);               // отпускаем замок
    return 0;
}


// Это главная структура всего драйвера
// Здесь мы говорим ядру: "вот мои функции, вот таблица устройств"
static struct usb_driver skel_driver = {
    .name =           "skeleton",               // имя драйвера (видно в lsmod)
    .probe =          skel_probe,               // функция, когда устройство подключили
    .disconnect =     skel_disconnect,          // функция, когда отключили
    .suspend =        skel_suspend,             // засыпание
    .resume =         skel_resume,              // пробуждение
    .pre_reset =      skel_pre_reset,           // перед сбросом
    .post_reset =     skel_post_reset,          // после сброса
    .id_table =       skel_table,               // таблица VID:PID
    .supports_autosuspend = 1,                  // разрешаем устройству "засыпать" автоматически
};

// Эта строчка — волшебство!
// Она автоматически регистрирует драйвер в ядре при загрузке модуля
module_usb_driver(skel_driver);


// Говорим ядру: "этот модуль под GPL v2 лицензией"
MODULE_LICENSE("GPL v2");
