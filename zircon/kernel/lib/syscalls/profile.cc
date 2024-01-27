// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <inttypes.h>
#include <lib/counters.h>
#include <lib/ktrace.h>
#include <lib/syscalls/forward.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <fbl/ref_ptr.h>
#include <object/handle.h>
#include <object/job_dispatcher.h>
#include <object/profile_dispatcher.h>
#include <object/vm_address_region_dispatcher.h>

KCOUNTER(profile_create, "profile.create")
KCOUNTER(profile_set, "profile.set")

// zx_status_t zx_profile_create
zx_status_t sys_profile_create(zx_handle_t root_job, uint32_t options,
                               user_in_ptr<const zx_profile_info_t> user_profile_info,
                               user_out_handle* out) {
  auto up = ProcessDispatcher::GetCurrent();

  zx_status_t status = up->EnforceBasicPolicy(ZX_POL_NEW_PROFILE);
  if (status != ZX_OK) {
    return status;
  }

  if (options != 0u) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::RefPtr<JobDispatcher> job;
  status = up->handle_table().GetDispatcherWithRights(*up, root_job, ZX_RIGHT_MANAGE_PROCESS, &job);
  if (status != ZX_OK) {
    return status;
  }

  if (job != GetRootJobDispatcher()) {
    // TODO(cpu): consider a better error code.
    return ZX_ERR_ACCESS_DENIED;
  }

  zx_profile_info_t profile_info;
  status = user_profile_info.copy_from_user(&profile_info);
  if (status != ZX_OK) {
    return status;
  }

  KernelHandle<ProfileDispatcher> handle;
  zx_rights_t rights;
  status = ProfileDispatcher::Create(profile_info, &handle, &rights);
  if (status != ZX_OK) {
    return status;
  }

  kcounter_add(profile_create, 1);

  return out->make(ktl::move(handle), rights);
}

// zx_status_t zx_object_set_profile
zx_status_t sys_object_set_profile(zx_handle_t handle, zx_handle_t profile_handle,
                                   uint32_t options) {
  auto up = ProcessDispatcher::GetCurrent();

  kcounter_add(profile_set, 1);

  fbl::RefPtr<ProfileDispatcher> profile;
  zx_status_t result = up->handle_table().GetDispatcherWithRights(*up, profile_handle,
                                                                  ZX_RIGHT_APPLY_PROFILE, &profile);
  if (result != ZX_OK)
    return result;

  fbl::RefPtr<Dispatcher> disp;
  zx_rights_t rights;
  result = up->handle_table().GetDispatcherWithRights(*up, handle, 0, &disp, &rights);
  if (result != ZX_OK)
    return result;

  if (fbl::RefPtr<ThreadDispatcher> thread = DownCastDispatcher<ThreadDispatcher>(&disp)) {
    if (!(rights & ZX_RIGHT_MANAGE_THREAD)) {
      return ZX_ERR_ACCESS_DENIED;
    }
    return profile->ApplyProfile(ktl::move(thread));
  }
  if (fbl::RefPtr<VmAddressRegionDispatcher> vmar =
          DownCastDispatcher<VmAddressRegionDispatcher>(&disp)) {
    if (!(rights & ZX_RIGHT_OP_CHILDREN)) {
      return ZX_ERR_ACCESS_DENIED;
    }
    return profile->ApplyProfile(ktl::move(vmar));
  }
  return ZX_ERR_WRONG_TYPE;
}
