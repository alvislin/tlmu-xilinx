/*
 * QEMU Synchronous Serial Interface support
 *
 * Copyright (c) 2009 CodeSourcery.
 * Copyright (c) 2012 Peter A.G. Crosthwaite (peter.crosthwaite@petalogix.com)
 * Copyright (c) 2012 PetaLogix Pty Ltd.
 * Written by Paul Brook
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "hw/ssi.h"

struct SSIBus {
    BusState qbus;
};

#define TYPE_SSI_BUS "SSI"
#define SSI_BUS(obj) OBJECT_CHECK(SSIBus, (obj), TYPE_SSI_BUS)

static const TypeInfo ssi_bus_info = {
    .name = TYPE_SSI_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(SSIBus),
};

static void ssi_cs_default(void *opaque, int n, int level)
{
    SSISlave *s = SSI_SLAVE(opaque);
    bool cs = !!level;
    assert(n == 0);
    if (s->cs != cs) {
        SSISlaveClass *ssc = SSI_SLAVE_GET_CLASS(s);
        if (ssc->set_cs) {
            ssc->set_cs(s, cs);
        }
    }
    s->cs = cs;
}

static uint32_t ssi_transfer_raw_default(SSISlave *dev, uint32_t val)
{
    SSISlaveClass *ssc = SSI_SLAVE_GET_CLASS(dev);

    if ((dev->cs && ssc->cs_polarity == SSI_CS_HIGH) ||
            (!dev->cs && ssc->cs_polarity == SSI_CS_LOW) ||
            ssc->cs_polarity == SSI_CS_NONE) {
        return ssc->transfer(dev, val);
    }
    return 0;
}

static int ssi_slave_init(DeviceState *dev)
{
    SSISlave *s = SSI_SLAVE(dev);
    SSISlaveClass *ssc = SSI_SLAVE_GET_CLASS(s);

    if (ssc->transfer_raw == ssi_transfer_raw_default &&
            ssc->cs_polarity != SSI_CS_NONE) {
        qdev_init_gpio_in(&s->qdev, ssi_cs_default, 1);
    }

    return ssc->init(s);
}

static void ssi_slave_class_init(ObjectClass *klass, void *data)
{
    SSISlaveClass *ssc = SSI_SLAVE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->init = ssi_slave_init;
    dc->bus_type = TYPE_SSI_BUS;
    if (!ssc->transfer_raw) {
        ssc->transfer_raw = ssi_transfer_raw_default;
    }
}

static const TypeInfo ssi_slave_info = {
    .name = TYPE_SSI_SLAVE,
    .parent = TYPE_DEVICE,
    .class_init = ssi_slave_class_init,
    .class_size = sizeof(SSISlaveClass),
    .abstract = true,
};

DeviceState *ssi_create_slave_no_init(SSIBus *bus, const char *name)
{
    return qdev_create(&bus->qbus, name);
}

DeviceState *ssi_create_slave(SSIBus *bus, const char *name)
{
    DeviceState *dev = ssi_create_slave_no_init(bus, name);

    qdev_init_nofail(dev);
    return dev;
}

SSIBus *ssi_create_bus(DeviceState *parent, const char *name)
{
    BusState *bus;
    bus = qbus_create(TYPE_SSI_BUS, parent, name);
    return FROM_QBUS(SSIBus, bus);
}

uint32_t ssi_transfer(SSIBus *bus, uint32_t val)
{
    BusChild *kid;
    SSISlaveClass *ssc;
    uint32_t r = 0;

    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        SSISlave *slave = SSI_SLAVE(kid->child);
        ssc = SSI_SLAVE_GET_CLASS(slave);
        r |= ssc->transfer_raw(slave, val);
    }

    return r;
}

const VMStateDescription vmstate_ssi_slave = {
    .name = "SSISlave",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_BOOL(cs, SSISlave),
        VMSTATE_END_OF_LIST()
    }
};

static void ssi_slave_register_types(void)
{
    type_register_static(&ssi_bus_info);
    type_register_static(&ssi_slave_info);
}

type_init(ssi_slave_register_types)

typedef struct SSIAutoConnectArg {
    qemu_irq *cs_linep;
    SSIBus *bus;
    int current;
    int first;
    int num;
} SSIAutoConnectArg;

static int ssi_auto_connect_slave(Object *child, void *opaque)
{
    SSIAutoConnectArg *arg = opaque;
    SSISlave *dev = (SSISlave *)object_dynamic_cast(child, TYPE_SSI_SLAVE);
    qemu_irq cs_line;

    if (!dev) {
        return 0;
    }

    if (arg->current >= arg->first && arg->current < arg->first + arg->num) {
        cs_line = qdev_get_gpio_in(DEVICE(dev), 0);
        qdev_set_parent_bus(DEVICE(dev), &arg->bus->qbus);
        *arg->cs_linep = cs_line;
    }
    arg->current++;
    arg->cs_linep++;
    return 0;
}

void ssi_auto_connect_slaves(DeviceState *parent, qemu_irq *cs_line,
                             SSIBus *bus, int first, int num)
{
    SSIAutoConnectArg arg = {
        .cs_linep = cs_line,
        .bus = bus,
        .current = 0,
        .first = first,
        .num = num,
    };

    object_child_foreach(OBJECT(parent), ssi_auto_connect_slave, &arg);
}
