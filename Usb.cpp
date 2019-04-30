/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "android.hardware.usb@1.1-hal-nexell"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include "Usb.h"

namespace android {
namespace hardware {
namespace usb {
namespace V1_1 {
namespace implementation {

#define GADGET_PATH				"/sys/devices/platform/c0000000.soc/"
#define GADGET_NAME				"c0040000.dwc2otg"
#define PULL_PATH GADGET_PATH GADGET_NAME
#define MODE_CHANGE PULL_PATH			"/sel_dr_mode"

int32_t readFile(const std::string &filename, std::string *contents)
{
	FILE *fp;
	ssize_t read = 0;
	char *line = NULL;
	size_t len = 0;

	fp = fopen(filename.c_str(), "r");
	if (fp != NULL) {
		if ((read = getline(&line, &len, fp)) != -1) {
			char *pos;
			if ((pos = strchr(line, '\n')) != NULL) *pos = '\0';
				*contents = line;
		}
		free(line);
		fclose(fp);
		return 0;
	} else {
		ALOGE("[%s] fopen failed", __func__);
	}

	return -1;
}

std::string convertRoletoString(PortRole role)
{
	if (role.type == PortRoleType::POWER_ROLE) {
		if (role.role == static_cast<uint32_t>(PortPowerRole::SOURCE))
			return "source";
		else if (role.role == static_cast<uint32_t>(PortPowerRole::SINK))
			return "sink";
	} else if (role.type == PortRoleType::DATA_ROLE) {
		if (role.role == static_cast<uint32_t>(PortDataRole::HOST))
			return "host";
		if (role.role == static_cast<uint32_t>(PortDataRole::DEVICE))
			return "device";
	} else if (role.type == PortRoleType::MODE) {
		if (role.role == static_cast<uint32_t>(PortMode_1_1::UFP))
			return "sink";
		if (role.role == static_cast<uint32_t>(PortMode_1_1::DFP))
			return "source";
	}
	return "none";
}

Usb::Usb()
{
	ALOGD("USB HAL started");
}

Return<void> Usb::switchRole(
		const hidl_string &portName,
		const V1_0::PortRole &newRole)
{
	std::string written;
	FILE *fp;
	bool roleSwitch = false;
	std::string filename(MODE_CHANGE);

	ALOGD("[%s] portName:%s, filename.c_str:%s, newRole:%s", __func__,
			filename.c_str(),
			portName.c_str(),
			convertRoletoString(newRole).c_str());
	if (filename == "") {
		ALOGE("[%s] Fatal: invalid node type", __func__);
		return Void();
	}

	if (newRole.type == PortRoleType::DATA_ROLE) {
		fp = fopen(filename.c_str(), "w");
		if (fp != NULL) {
			int ret = fputs(convertRoletoString(newRole).c_str(), fp);
			fclose(fp);
			if ((ret != EOF) && !readFile(filename, &written)) {
				if (written == convertRoletoString(newRole)) {
					roleSwitch = true;
				} else {
					ALOGE("[%s] Role switch failed", __func__);
				}
			} else {
				ALOGE("[%s] failed to update the new role", __func__);
			}
		} else {
			ALOGE("[%s] open failed for %s", __func__, filename.c_str());
		}
	}

	if (mCallback_1_0 != NULL) {
		Return<void> ret =
			mCallback_1_0->notifyRoleSwitchStatus(portName, newRole,
					roleSwitch ? Status::SUCCESS : Status::ERROR);
		if (!ret.isOk())
			ALOGE("[%s] RoleSwitchStatus error %s", __func__,
					ret.description().c_str());
		else
			queryPortStatus();
	} else {
		ALOGE("[%s] Not notifying the userspace. Callback is not set", __func__);
	}

	return Void();
}

PortDataRole getCurrentRole(void)
{
	std::string current_mode;
	PortDataRole mode = PortDataRole::NONE;

	if (!readFile(MODE_CHANGE, &current_mode)) {
		ALOGE("[%s] current_mode:%s", __func__, current_mode.c_str());
		if (current_mode == "host")
			mode = PortDataRole::HOST;
		else if (current_mode == "device")
			mode = PortDataRole::DEVICE;
	} else
		ALOGE("[%s] Failed to read %s", __func__, MODE_CHANGE);
	return mode;
}

/*
 * Reuse the same method for both V1_0 and V1_1 callback objects.
 * The caller of this method would reconstruct the V1_0::PortStatus
 * object if required.
 */
Status getPortStatusHelper(hidl_vec<PortStatus_1_1> *currentPortStatus_1_1,
    bool V1_0)
{
	PortDataRole mode = getCurrentRole();

	ALOGD("[%s]", __func__);
	if (mode == PortDataRole::NONE)
		return Status::ERROR;

	(*currentPortStatus_1_1)[0].status.portName = "otg";
	(*currentPortStatus_1_1)[0].status.currentDataRole = mode;
	(*currentPortStatus_1_1)[0].status.currentPowerRole =
		(mode == PortDataRole::DEVICE) ?
		PortPowerRole::SINK : PortPowerRole::SOURCE;
	(*currentPortStatus_1_1)[0].status.currentMode = V1_0::PortMode::DRP;
	(*currentPortStatus_1_1)[0].status.canChangeMode = false;
	(*currentPortStatus_1_1)[0].status.canChangeDataRole = true;
	(*currentPortStatus_1_1)[0].status.canChangePowerRole = true;

	ALOGD("canChangeMode:%d canChagedata:%d canChangePower:%d",
		(*currentPortStatus_1_1)[0].status.canChangeMode,
		(*currentPortStatus_1_1)[0].status.canChangeDataRole,
		(*currentPortStatus_1_1)[0].status.canChangePowerRole);

	if (V1_0) {
		(*currentPortStatus_1_1)[0].status.supportedModes = V1_0::PortMode::DRP;
	} else {
		(*currentPortStatus_1_1)[0].status.supportedModes = V1_0::PortMode::DRP;
		(*currentPortStatus_1_1)[0].supportedModes =
			PortMode_1_1::UFP | PortMode_1_1::DFP;
		(*currentPortStatus_1_1)[0].currentMode = PortMode_1_1::DRP;
	}
	return Status::SUCCESS;
}

Return<void> Usb::queryPortStatus()
{
	hidl_vec<PortStatus_1_1> currentPortStatus_1_1;
	hidl_vec<V1_0::PortStatus> currentPortStatus;
	sp<IUsbCallback> callback_V1_1 = IUsbCallback::castFrom(mCallback_1_0);
	Status status;

	ALOGD("[%s]", __func__);

	currentPortStatus.resize(1);
	currentPortStatus_1_1.resize(1);
	if (mCallback_1_0 != NULL) {
		if (callback_V1_1 != NULL) {
			status = getPortStatusHelper(&currentPortStatus_1_1, false);
		} else {
			status = getPortStatusHelper(&currentPortStatus_1_1, true);
			currentPortStatus[0] = currentPortStatus_1_1[0].status;
		}

		Return<void> ret;
		if (callback_V1_1 != NULL)
			ret = callback_V1_1->notifyPortStatusChange_1_1(currentPortStatus_1_1, status);
		else
			ret = mCallback_1_0->notifyPortStatusChange(currentPortStatus, status);

		if (!ret.isOk())
			ALOGE("[%s] queryPortStatus_1_1 error %s", __func__,
					ret.description().c_str());
	} else {
		ALOGD("Notifying userspace skipped. Callback is NULL");
	}

	return Void();
}

Return<void> Usb::setCallback(const sp<V1_0::IUsbCallback> &callback)
{
	sp<IUsbCallback> callback_V1_1 = IUsbCallback::castFrom(callback);

	if (callback != NULL)
		if (callback_V1_1 == NULL)
			ALOGD("Registering 1.0 callback");

	/*
	* When both the old callback and new callback values are NULL,
	* there is no need to spin off the worker thread.
	* When both the values are not NULL, we would already have a
	* worker thread running, so updating the callback object would
	* be suffice.
	*/
	if ((mCallback_1_0 == NULL && callback == NULL) ||
			(mCallback_1_0 != NULL && callback != NULL)) {
		/*
		* Always store as V1_0 callback object. Type cast to V1_1
		* when the callback is actually invoked.
		*/
		mCallback_1_0 = callback;
		return Void();
	}

	mCallback_1_0 = callback;
	ALOGD("registering callback");

	return Void();
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace usb
}  // namespace hardware
}  // namespace android
