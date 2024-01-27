// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SYSMEM_CONNECTOR_SYSMEM_CONNECTOR_H_
#define LIB_SYSMEM_CONNECTOR_SYSMEM_CONNECTOR_H_

#include <zircon/compiler.h>
#include <zircon/types.h>

__BEGIN_CDECLS

// The client of sysmem-connector shouldn't need to know how large this struct
// is (or even whether the pointer is to a heap allocation vs. within a heap
// allocation), so it's declared here, but defined in sysmem-connector.cpp.
typedef struct sysmem_connector sysmem_connector_t;

// Allocate and initialize a sysmem_connector_t.  ZX_OK from this function
// doesn't guarantee that the sysmem driver is found yet, only that the
// connector has successfully been created and initialized.
//
// |sysmem_directory_path| must remain valid for lifetime of sysmem_connector_t.
// This is the path to the directory of sysmem device instances (just one
// device instance will actually exist, unless something is going wrong).
//
// |terminate_on_sysmem_connection_failure| true will terminate this process if
// the initial persistent connection to sysmem ever closes from the remote end
// due to sysmem failure.  Passing false will instead attempt to re-connect to
// sysmem.  System-wide, in typical configurations, there will be at least one
// critical process that passes true here, so that the system will reboot on
// sysmem failure (currently with reason "fuchsia-reboot-" +
// <critical_process_name> + "-terminated".
//
// TODO()
__EXPORT zx_status_t sysmem_connector_init(const char* sysmem_directory_path,
                                           bool terminate_on_sysmem_connection_failure,
                                           sysmem_connector_t** out_connector);

// allocator_request is consumed.  A call to this function doesn't guarantee
// that the request will reach the sysmem driver, only that the connector has
// queued the request internally to be sent.
//
// If the sysmem driver can't be contacted for an extended duration, the request
// may sit in the queue for that duration - there isn't a timeout, because that
// would probably do more harm than good, since sysmem is always supposed to be
// running.
__EXPORT void sysmem_connector_queue_connection_request_v1(sysmem_connector_t* connector,
                                                           zx_handle_t allocator_request);
__EXPORT void sysmem_connector_queue_connection_request_v2(sysmem_connector_t* connector,
                                                           zx_handle_t allocator_request);

// Sysmem needs access to Cobalt.  We provide a service directory to sysmem which has only Cobalt
// in it.
__EXPORT void sysmem_connector_queue_service_directory(sysmem_connector_t* connector_param,
                                                       zx_handle_t service_directory_param);

// This call is not allowed to fail.
__EXPORT void sysmem_connector_release(sysmem_connector_t* connector);

__END_CDECLS

#endif  // LIB_SYSMEM_CONNECTOR_SYSMEM_CONNECTOR_H_
