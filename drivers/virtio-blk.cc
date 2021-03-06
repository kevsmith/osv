/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */


#include <sys/cdefs.h>

#include "drivers/virtio.hh"
#include "drivers/virtio-blk.hh"
#include "drivers/pci-device.hh"
#include "interrupt.hh"

#include "mempool.hh"
#include "mmu.hh"

#include <sstream>
#include <string>
#include <string.h>
#include <map>
#include <errno.h>
#include <osv/debug.h>

#include "sched.hh"
#include "osv/trace.hh"
#include "drivers/clock.hh"
#include "drivers/clockevent.hh"

#include <osv/device.h>
#include <osv/bio.h>

TRACEPOINT(trace_virtio_blk_read_config_capacity, "capacity=%lu", u64);
TRACEPOINT(trace_virtio_blk_read_config_size_max, "size_max=%u", u32);
TRACEPOINT(trace_virtio_blk_read_config_seg_max, "seg_max=%u", u32);
TRACEPOINT(trace_virtio_blk_read_config_geometry, "cylinders=%u, heads=%u, sectors=%u", u32, u32, u32);
TRACEPOINT(trace_virtio_blk_read_config_blk_size, "blk_size=%u", u32);
TRACEPOINT(trace_virtio_blk_read_config_topology, "physical_block_exp=%u, alignment_offset=%u, min_io_size=%u, opt_io_size=%u", u32, u32, u32, u32);
TRACEPOINT(trace_virtio_blk_read_config_wce, "wce=%u", u32);
TRACEPOINT(trace_virtio_blk_read_config_ro, "readonly=true");
TRACEPOINT(trace_virtio_blk_make_virtio_request_seg_max, "request of size %d needs more segment than the max %d", size_t, u32);
TRACEPOINT(trace_virtio_blk_make_virtio_request_readonly, "write on readonly device");
TRACEPOINT(trace_virtio_blk_wake, "");
TRACEPOINT(trace_virtio_blk_strategy, "bio=%p", struct bio*);
TRACEPOINT(trace_virtio_blk_req_ok, "bio=%p, sector=%lu, len=%lu, type=%x", struct bio*, u64, size_t, u32);
TRACEPOINT(trace_virtio_blk_req_unsupp, "bio=%p, sector=%lu, len=%lu, type=%x", struct bio*, u64, size_t, u32);
TRACEPOINT(trace_virtio_blk_req_err, "bio=%p, sector=%lu, len=%lu, type=%x", struct bio*, u64, size_t, u32);

using namespace memory;


namespace virtio {

int virtio_blk::_instance = 0;


struct virtio_blk_priv {
    virtio_blk* drv;
};

static void
virtio_blk_strategy(struct bio *bio)
{
    struct virtio_blk_priv *prv = reinterpret_cast<struct virtio_blk_priv*>(bio->bio_dev->private_data);

    trace_virtio_blk_strategy(bio);
    bio->bio_offset += bio->bio_dev->offset;
    prv->drv->make_virtio_request(bio);
}

static int
virtio_blk_read(struct device *dev, struct uio *uio, int ioflags)
{
    if (uio->uio_offset + uio->uio_resid > dev->size)
        return EIO;

    return bdev_read(dev, uio, ioflags);
}

static int
virtio_blk_write(struct device *dev, struct uio *uio, int ioflags)
{
    auto* prv = reinterpret_cast<struct virtio_blk_priv*>(dev->private_data);

    if (prv->drv->is_readonly()) return EROFS;
    if (uio->uio_offset + uio->uio_resid > dev->size)
        return EIO;

    return bdev_write(dev, uio, ioflags);
}

static struct devops virtio_blk_devops {
    no_open,
    no_close,
    virtio_blk_read,
    virtio_blk_write,
    no_ioctl,
    no_devctl,
    virtio_blk_strategy,
};

struct driver virtio_blk_driver = {
    "virtio_blk",
    &virtio_blk_devops,
    sizeof(struct virtio_blk_priv),
};

virtio_blk::virtio_blk(pci::device& pci_dev)
    : virtio_driver(pci_dev), _ro(false)
{
    std::stringstream ss;
    ss << "virtio-blk";

    _driver_name = ss.str();
    _id = _instance++;
    virtio_i("VIRTIO BLK INSTANCE %d", _id);

    setup_features();
    read_config();

    //register the single irq callback for the block
    sched::thread* t = new sched::thread([this] { this->response_worker(); });
    t->start();
    auto queue = get_virt_queue(0);
    _msi.easy_register({ { 0, [=] { queue->disable_interrupts(); }, t } });

    add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);

    struct virtio_blk_priv* prv;
    struct device *dev;
    std::string dev_name("vblk");
    dev_name += std::to_string(_id);

    dev = device_create(&virtio_blk_driver, dev_name.c_str(), D_BLK);
    prv = reinterpret_cast<struct virtio_blk_priv*>(dev->private_data);
    prv->drv = this;
    dev->size = prv->drv->size();
    read_partition_table(dev);
}

virtio_blk::~virtio_blk()
{
    //TODO: In theory maintain the list of free instances and gc it
    // including the thread objects and their stack
}

bool virtio_blk::read_config()
{
    //read all of the block config (including size, mce, topology,..) in one shot
    virtio_conf_read(virtio_pci_config_offset(), &_config, sizeof(_config));

    trace_virtio_blk_read_config_capacity(_config.capacity);

    if (get_guest_feature_bit(VIRTIO_BLK_F_SIZE_MAX))
        trace_virtio_blk_read_config_size_max(_config.size_max);
    if (get_guest_feature_bit(VIRTIO_BLK_F_SEG_MAX))
        trace_virtio_blk_read_config_seg_max(_config.seg_max);
    if (get_guest_feature_bit(VIRTIO_BLK_F_GEOMETRY)) {
        trace_virtio_blk_read_config_geometry((u32)_config.geometry.cylinders, (u32)_config.geometry.heads, (u32)_config.geometry.sectors);
    }
    if (get_guest_feature_bit(VIRTIO_BLK_F_BLK_SIZE))
        trace_virtio_blk_read_config_blk_size(_config.blk_size);
    if (get_guest_feature_bit(VIRTIO_BLK_F_TOPOLOGY)) {
        trace_virtio_blk_read_config_topology((u32)_config.physical_block_exp, (u32)_config.alignment_offset, (u32)_config.min_io_size, (u32)_config.opt_io_size);
    }
    if (get_guest_feature_bit(VIRTIO_BLK_F_CONFIG_WCE))
        trace_virtio_blk_read_config_wce((u32)_config.wce);
    if (get_guest_feature_bit(VIRTIO_BLK_F_RO)) {
        set_readonly();
        trace_virtio_blk_read_config_ro();
    }

    return true;
}

void virtio_blk::response_worker()
{
    auto* queue = get_virt_queue(0);
    virtio_blk_req* req;

    while (1) {

        virtio_driver::wait_for_queue(queue, &vring::used_ring_not_empty);
        trace_virtio_blk_wake();

        u32 len;
        while((req = static_cast<virtio_blk_req*>(queue->get_buf_elem(&len))) != nullptr) {
            if (req->bio) {
                switch (req->res.status) {
                case VIRTIO_BLK_S_OK:
                    biodone(req->bio, true);
                    trace_virtio_blk_req_ok(req->bio, req->hdr.sector, req->bio->bio_bcount, req->hdr.type);
                    break;
                case VIRTIO_BLK_S_UNSUPP:
                    biodone(req->bio, false);
                    trace_virtio_blk_req_unsupp(req->bio, req->hdr.sector, req->bio->bio_bcount, req->hdr.type);
                    break;
                default:
                    biodone(req->bio, false);
                    trace_virtio_blk_req_err(req->bio, req->hdr.sector, req->bio->bio_bcount, req->hdr.type);
                    break;
               }
            }

            req->bio = nullptr;
            delete req;
            queue->get_buf_finalize();
        }

        // wake up the requesting thread in case the ring was full before
        _request_thread_lock.lock();
        if (_waiting_request_thread) {
           _waiting_request_thread->wake_with([&] { _request_thread_lock.unlock(); });
        } else {
            _request_thread_lock.unlock();
        }
    }
}

int64_t virtio_blk::size()
{
    return _config.capacity * _config.blk_size;
}

static const int sector_size = 512;

int virtio_blk::make_virtio_request(struct bio* bio)
{
    // The lock is here for parallel requests protection
    WITH_LOCK(_lock) {

        if (!bio) return EIO;

        if (bio->bio_bcount/mmu::page_size + 1 > _config.seg_max) {
            trace_virtio_blk_make_virtio_request_seg_max(bio->bio_bcount, _config.seg_max);
            return EIO;
        }

        auto* queue = get_virt_queue(0);
        virtio_blk_request_type type;

        switch (bio->bio_cmd) {
        case BIO_READ:
            type = VIRTIO_BLK_T_IN;
            break;
        case BIO_WRITE:
            if (is_readonly()) {
                trace_virtio_blk_make_virtio_request_readonly();
                biodone(bio, false);
                return EROFS;
            }
            type = VIRTIO_BLK_T_OUT;
            break;
        case BIO_FLUSH:
            type = VIRTIO_BLK_T_FLUSH;
            break;
        default:
            return ENOTBLK;
        }

        auto* req = new virtio_blk_req;
        req->bio = bio;
        virtio_blk_outhdr* hdr = &req->hdr;
        hdr->type = type;
        hdr->ioprio = 0;
        hdr->sector = bio->bio_offset / sector_size;

        queue->_sg_vec.clear();
        queue->_sg_vec.push_back(vring::sg_node(mmu::virt_to_phys(hdr), sizeof(struct virtio_blk_outhdr), vring_desc::VRING_DESC_F_READ));

        // need to break a contiguous buffers that > 4k into several physical page mapping
        // even if the virtual space is contiguous.
        size_t len = 0;
        auto offset = reinterpret_cast<size_t>(bio->bio_data) & 0xfff;
        auto *base = bio->bio_data;
        while (len != bio->bio_bcount) {
            auto size = std::min(bio->bio_bcount - len, mmu::page_size);
            if (offset + size > mmu::page_size)
                size = mmu::page_size - offset;
            len += size;
            queue->_sg_vec.push_back(vring::sg_node(mmu::virt_to_phys(base), size, (type == VIRTIO_BLK_T_OUT)? vring_desc::VRING_DESC_F_READ:vring_desc::VRING_DESC_F_WRITE));
            base += size;
            offset = 0;
        }

        req->res.status = 0;
        queue->_sg_vec.push_back(vring::sg_node(mmu::virt_to_phys(&req->res), sizeof (struct virtio_blk_res), vring_desc::VRING_DESC_F_WRITE));

        while (!queue->add_buf(req)) {
            _waiting_request_thread = sched::thread::current();
            std::atomic_thread_fence(std::memory_order_seq_cst);
            while (!queue->avail_ring_has_room(queue->_sg_vec.size())) {
                sched::thread::wait_until([queue] {return queue->used_ring_can_gc();});
                queue->get_buf_gc();
            }
            WITH_LOCK(_request_thread_lock) {
                _waiting_request_thread = nullptr;
            }
        }

        queue->kick();

        return 0;
    }
}

u32 virtio_blk::get_driver_features(void)
{
    auto base = virtio_driver::get_driver_features();
    return (base | ( 1 << VIRTIO_BLK_F_SIZE_MAX)
                 | ( 1 << VIRTIO_BLK_F_SEG_MAX)
                 | ( 1 << VIRTIO_BLK_F_GEOMETRY)
                 | ( 1 << VIRTIO_BLK_F_RO)
                 | ( 1 << VIRTIO_BLK_F_BLK_SIZE)
                 | ( 1 << VIRTIO_BLK_F_CONFIG_WCE)
                 | ( 1 << VIRTIO_BLK_F_WCE));
}

hw_driver* virtio_blk::probe(hw_device* dev)
{
    return virtio::probe<virtio_blk, VIRTIO_BLK_DEVICE_ID>(dev);
}

}
