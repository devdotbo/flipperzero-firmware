#include "event_dispatcher.h"
#include <core/check.h>
#include <furi.h>
#include <ble/ble.h>

#include <m-list.h>

struct GapEventHandler {
    void* context;
    BleSvcEventHandlerCb callback;
};

LIST_DEF(GapSvcEventHandlerList, GapSvcEventHandler, M_POD_OPLIST);

static GapSvcEventHandlerList_t handlers;
static bool initialized = false;
/* Serialises list traversal on the BLE event thread against register/unregister,
 * so that a handler that has been unregistered is never called afterwards and its
 * list node is never read after it was freed. Held only for list operations and
 * the handler loop; ble_event_app_notification() runs without it. */
static FuriMutex* handlers_mutex = NULL;

BleEventFlowStatus ble_event_dispatcher_process_event(void* payload) {
    furi_check(initialized);

    GapSvcEventHandlerList_it_t it;
    BleEventAckStatus ack_status = BleEventNotAck;

    furi_check(furi_mutex_acquire(handlers_mutex, FuriWaitForever) == FuriStatusOk);
    for(GapSvcEventHandlerList_it(it, handlers); !GapSvcEventHandlerList_end_p(it);
        GapSvcEventHandlerList_next(it)) {
        const GapSvcEventHandler* item = GapSvcEventHandlerList_cref(it);
        ack_status = item->callback(payload, item->context);
        if(ack_status == BleEventNotAck) {
            /* Keep going */
            continue;
        } else if((ack_status == BleEventAckFlowEnable) || (ack_status == BleEventAckFlowDisable)) {
            break;
        }
    }
    furi_check(furi_mutex_release(handlers_mutex) == FuriStatusOk);

    /* Now, decide on a flow control action based on results of all handlers */
    switch(ack_status) {
    case BleEventNotAck:
        /* The event has NOT been managed yet. Pass to app for processing */
        return ble_event_app_notification(payload);
    case BleEventAckFlowEnable:
        return BleEventFlowEnable;
    case BleEventAckFlowDisable:
        return BleEventFlowDisable;
    default:
        return BleEventFlowEnable;
    }
}

void ble_event_dispatcher_init(void) {
    if(!initialized) {
        GapSvcEventHandlerList_init(handlers);
        handlers_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
        initialized = true;
    }
}

void ble_event_dispatcher_reset(void) {
    furi_check(initialized);
    furi_check(furi_mutex_acquire(handlers_mutex, FuriWaitForever) == FuriStatusOk);
    furi_check(GapSvcEventHandlerList_size(handlers) == 0);

    GapSvcEventHandlerList_clear(handlers);
    furi_check(furi_mutex_release(handlers_mutex) == FuriStatusOk);
}

GapSvcEventHandler*
    ble_event_dispatcher_register_svc_handler(BleSvcEventHandlerCb handler, void* context) {
    furi_check(handler);
    furi_check(context);
    furi_check(initialized);

    furi_check(furi_mutex_acquire(handlers_mutex, FuriWaitForever) == FuriStatusOk);
    GapSvcEventHandler* item = GapSvcEventHandlerList_push_raw(handlers);
    item->context = context;
    item->callback = handler;
    furi_check(furi_mutex_release(handlers_mutex) == FuriStatusOk);

    return item;
}

void ble_event_dispatcher_unregister_svc_handler(GapSvcEventHandler* handler) {
    furi_check(handler);
    furi_check(initialized);
    /* Called from inside a handler on the BLE event thread this would deadlock. */
    furi_check(
        furi_mutex_get_owner(handlers_mutex) != furi_thread_get_current_id(),
        "BLE handler unregistered from the BLE event thread");

    bool found = false;
    GapSvcEventHandlerList_it_t it;

    furi_check(furi_mutex_acquire(handlers_mutex, FuriWaitForever) == FuriStatusOk);

    for(GapSvcEventHandlerList_it(it, handlers); !GapSvcEventHandlerList_end_p(it);
        GapSvcEventHandlerList_next(it)) {
        const GapSvcEventHandler* item = GapSvcEventHandlerList_cref(it);

        if(item == handler) {
            GapSvcEventHandlerList_remove(handlers, it);
            found = true;
            break;
        }
    }
    furi_check(furi_mutex_release(handlers_mutex) == FuriStatusOk);

    furi_check(found);
}
