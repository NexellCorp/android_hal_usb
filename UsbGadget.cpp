/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "android.hardware.usb.gadget@1.0-hal-nexell"

#include "UsbGadget.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

constexpr int MAX_FILE_PATH_LENGTH = 256;
constexpr int DISCONNECT_WAIT_US = 100000;
constexpr int PULL_UP_DELAY = 500000;

#define BUILD_TYPE				"ro.build.type"
#define GADGET_PATH				"/config/usb_gadget/g1/"
#define PULLUP_PATH GADGET_PATH			"UDC"
#define GADGET_NAME				"c0040000.dwc2otg"
#define VENDOR_ID_PATH GADGET_PATH		"idVendor"
#define PRODUCT_ID_PATH GADGET_PATH		"idProduct"
#define DEVICE_CLASS_PATH GADGET_PATH		"bDeviceClass"
#define DEVICE_SUB_CLASS_PATH GADGET_PATH	"bDeviceSubClass"
#define DEVICE_PROTOCOL_PATH GADGET_PATH	"bDeviceProtocol"
#define DESC_USE_PATH GADGET_PATH		"os_desc/use"
#define OS_DESC_PATH GADGET_PATH		"os_desc/b.1"
#define CONFIG_PATH GADGET_PATH			"configs/b.1/"
#define FUNCTIONS_PATH GADGET_PATH		"functions/"
#define FUNCTION_NAME				"f"
#define FUNCTION_PATH CONFIG_PATH FUNCTION_NAME
#define RNDIS_PATH FUNCTIONS_PATH		"rndis.gs4"
#define STRING_PATH CONFIG_PATH			"strings/0x409/configuration"

#define VENDOR_ID				"0x18d1"
#define PRODUCT_ID_ADB				"0x4ee7"
#define PRODUCT_ID_MTP				"0x4ee1"
#define PRODUCT_ID_MTP_ADB			"0x4ee2"
#define PRODUCT_ID_RNDIS			"0x4ee3"
#define PRODUCT_ID_RNDIS_ADB			"0x4ee4"
#define PRODUCT_ID_PTP				"0x4ee5"
#define PRODUCT_ID_PTP_ADB			"0x4ee6"
#define PRODUCT_ID_MIDI				"0x4ee8"
#define PRODUCT_ID_MIDI_ADB			"0x4ee9"
#define PRODUCT_ID_ACC				"0x2d00"
#define PRODUCT_ID_ACC_ADB			"0x2d01"
#define PRODUCT_ID_AUDIO_SRC			"0x2d02"
#define PRODUCT_ID_AUDIO_SRC_ADB		"0x2d03"
#define PRODUCT_ID_AUDIO_SRC_ACC		"0x2d04"
#define PRODUCT_ID_AUDIO_SRC_ACC_ADB		"0x2d05"

namespace android {
namespace hardware {
namespace usb {
namespace gadget {
namespace V1_0 {
namespace implementation {

UsbGadget::UsbGadget()
	:mCurrentUsbFunctionsApplied(false)
{
	if (access(OS_DESC_PATH, R_OK) != 0)
		ALOGE("configfs setup not done yet");
}

static int unlinkFunctions(const char *path)
{
	DIR *config = opendir(path);
	struct dirent *function;
	char filepath[MAX_FILE_PATH_LENGTH];
	int ret = 0;

	if (config == NULL)
		return -1;

	// d_type does not seems to be supported in /config
	// so filtering by name.
	while (((function = readdir(config)) != NULL)) {
		if ((strstr(function->d_name, FUNCTION_NAME) == NULL))
			continue;
		// build the path for each file in the folder.
		sprintf(filepath, "%s%s", path, function->d_name);
		ret = remove(filepath);
		if (ret) {
			ALOGE("Unable  remove file %s errno:%d", filepath, errno);
			break;
		}
	}

	closedir(config);
	return ret;
}

Return<void> UsbGadget::getCurrentUsbFunctions(
		const sp<V1_0::IUsbGadgetCallback> &callback)
{
	Return<void> ret = callback->getCurrentUsbFunctionsCb(
			mCurrentUsbFunctions, mCurrentUsbFunctionsApplied
			? Status::FUNCTIONS_APPLIED
			: Status::FUNCTIONS_NOT_APPLIED);
	if (!ret.isOk())
		ALOGE("Call to getCurrentUsbFunctionsCb failed %s",
				ret.description().c_str());

	return Void();
}

V1_0::Status UsbGadget::tearDownGadget()
{
	if (!WriteStringToFile("none", PULLUP_PATH))
		ALOGD("Gadget cannot be pulled down");

	if (!WriteStringToFile("0", DEVICE_CLASS_PATH))
		return Status::ERROR;

	if (!WriteStringToFile("0", DEVICE_SUB_CLASS_PATH))
		return Status::ERROR;

	if (!WriteStringToFile("0", DEVICE_PROTOCOL_PATH))
		return Status::ERROR;

	if (!WriteStringToFile("0", DESC_USE_PATH))
		return Status::ERROR;

	if (unlinkFunctions(CONFIG_PATH))
		return Status::ERROR;

	return Status::SUCCESS;
}

static int linkFunction(const char *function, int index)
{
	char functionPath[MAX_FILE_PATH_LENGTH];
	char link[MAX_FILE_PATH_LENGTH];

	sprintf(functionPath, "%s%s", FUNCTIONS_PATH, function);
	sprintf(link, "%s%d", FUNCTION_PATH, index);
	if (symlink(functionPath, link)) {
		ALOGE("Cannot create symlink %s -> %s errno:%d", link, functionPath, errno);
		return -1;
	}
	return 0;
}

static V1_0::Status setVidPid(const char *vid, const char *pid) {
	if (!WriteStringToFile(vid, VENDOR_ID_PATH))
		return Status::ERROR;

	if (!WriteStringToFile(pid, PRODUCT_ID_PATH))
		return Status::ERROR;

	return Status::SUCCESS;
}

static V1_0::Status validateAndSetVidPid(uint64_t functions)
{
	V1_0::Status ret = Status::SUCCESS;

	switch (functions) {
	case static_cast<uint64_t>(GadgetFunction::MTP):
		ALOGI("[%s] MTP", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_MTP);
	break;
	case GadgetFunction::ADB | GadgetFunction::MTP:
		ALOGI("[%s] ADB | MTP", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_MTP_ADB);
	break;
	case static_cast<uint64_t>(GadgetFunction::RNDIS):
		ALOGI("[%s] RNDIS", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_RNDIS);
	break;
	case GadgetFunction::ADB | GadgetFunction::RNDIS:
		ALOGI("[%s] ADB | RNDIS", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_RNDIS_ADB);
	break;
	case static_cast<uint64_t>(GadgetFunction::PTP):
		ALOGI("[%s] PTP", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_PTP);
	break;
	case GadgetFunction::ADB | GadgetFunction::PTP:
		ALOGI("[%s] ADB | PTP", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_PTP_ADB);
	break;
	case static_cast<uint64_t>(GadgetFunction::ADB):
		ALOGI("[%s] ADB", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_ADB);
	break;
	case static_cast<uint64_t>(GadgetFunction::MIDI):
		ALOGI("[%s] MIDI", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_MIDI);
	break;
	case GadgetFunction::ADB | GadgetFunction::MIDI:
		ALOGI("[%s] ADB | MIDI", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_MIDI_ADB);
	break;
	case static_cast<uint64_t>(GadgetFunction::ACCESSORY):
		ALOGI("[%s] Accessory", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_ACC);
	break;
	case GadgetFunction::ADB | GadgetFunction::ACCESSORY:
		ALOGI("[%s] ADB | Accessory", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_ACC_ADB);
	break;
	case static_cast<uint64_t>(GadgetFunction::AUDIO_SOURCE):
		ALOGI("[%s] Audio Source", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_AUDIO_SRC);
	break;
	case GadgetFunction::ADB | GadgetFunction::AUDIO_SOURCE:
		ALOGI("[%s] ADB | Audio Source", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_AUDIO_SRC_ADB);
	break;
	case GadgetFunction::ACCESSORY | GadgetFunction::AUDIO_SOURCE:
		ALOGI("[%s] Accessory | Audio Source", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_AUDIO_SRC_ACC);
	break;
	case GadgetFunction::ADB | GadgetFunction::ACCESSORY |
		GadgetFunction::AUDIO_SOURCE:
		ALOGI("[%s] ADB | Accessory | Audio Source", __func__);
		ret = setVidPid(VENDOR_ID, PRODUCT_ID_AUDIO_SRC_ACC_ADB);
	break;
	default:
		ALOGE("Combination not supported");
		ret = Status::CONFIGURATION_NOT_SUPPORTED;
	}
	return ret;
}

V1_0::Status UsbGadget::setupFunctions(uint64_t functions,
		const sp<V1_0::IUsbGadgetCallback> &callback,
		uint64_t timeout)
{
	bool ffsEnabled = false;
	int i = 1;

	(void)(timeout);

	if (((functions & GadgetFunction::MTP) != 0)) {
		ALOGD("setCurrentUsbFunctions MTP");
		ffsEnabled = true;
		if (!WriteStringToFile("MTP", STRING_PATH))
			return Status::ERROR;

		if (linkFunction("mtp.gs0", i++))
			return Status::ERROR;

	} else if (((functions & GadgetFunction::PTP) != 0)) {
		ALOGD("setCurrentUsbFunctions PTP");
		ffsEnabled = true;
		if (!WriteStringToFile("PTP", STRING_PATH))
			return Status::ERROR;
		if (linkFunction("ptp.gs1", i++))
			return Status::ERROR;
	}

	if ((functions & GadgetFunction::MIDI) != 0) {
		ALOGD("setCurrentUsbFunctions MIDI");
		if (linkFunction("midi.gs5", i++))
			return Status::ERROR;
	}

	if ((functions & GadgetFunction::ACCESSORY) != 0) {
		ALOGD("setCurrentUsbFunctions Accessory");
		if (linkFunction("accessory.gs2", i++))
			return Status::ERROR;
	}

	if ((functions & GadgetFunction::AUDIO_SOURCE) != 0) {
		ALOGD("setCurrentUsbFunctions Audio Source");
		if (linkFunction("audio_source.gs3", i++))
			return Status::ERROR;
	}

	if ((functions & GadgetFunction::RNDIS) != 0) {
		ALOGD("setCurrentUsbFunctions rndis");
		if (linkFunction("rndis.gs4", i++))
			return Status::ERROR;
	}

	if ((functions & GadgetFunction::ADB) != 0) {
		ffsEnabled = true;
		ALOGD("setCurrentUsbFunctions Adb");
		if (linkFunction("ffs.adb", i++))
			return Status::ERROR;
		ALOGD("Service started");
	}

	// Pull up the gadget right away when there are no ffs functions.
	if (!ffsEnabled) {
		if (!WriteStringToFile(GADGET_NAME, PULLUP_PATH))
			return Status::ERROR;
		mCurrentUsbFunctionsApplied = true;
		if (callback)
			callback->setCurrentUsbFunctionsCb(functions, Status::SUCCESS);
		return Status::SUCCESS;
	}

	if (callback) {
		if (!!WriteStringToFile(GADGET_NAME, PULLUP_PATH)) {
			mCurrentUsbFunctionsApplied = true;
			usleep(PULL_UP_DELAY);
		} else
			mCurrentUsbFunctionsApplied = false;
		Return<void> ret = callback->setCurrentUsbFunctionsCb(
				functions,
				mCurrentUsbFunctionsApplied ? Status::SUCCESS : Status::ERROR);
		if (!ret.isOk())
			ALOGE("setCurrentUsbFunctionsCb error %s", ret.description().c_str());

		if (((functions & GadgetFunction::MTP) != 0) ||
				((functions & GadgetFunction::PTP) != 0)) {
			if (!WriteStringToFile("1", DESC_USE_PATH))
				return Status::ERROR;
		}
	}

	return Status::SUCCESS;
}

Return<void> UsbGadget::setCurrentUsbFunctions(
		uint64_t functions, const sp<V1_0::IUsbGadgetCallback> &callback,
		uint64_t timeout) {
	std::unique_lock<std::mutex> lk(mLockSetCurrentFunction);

	mCurrentUsbFunctions = functions;
	mCurrentUsbFunctionsApplied = false;

	ALOGD("[%s] function:%llu", __func__, functions);
	// Unlink the gadget and stop the monitor if running.
	V1_0::Status status = tearDownGadget();
		if (status != Status::SUCCESS) {
		goto error;
	}

	// Leave the gadget pulled down to give time for the host to sense disconnect.
	usleep(DISCONNECT_WAIT_US);

	if (functions == static_cast<uint64_t>(GadgetFunction::NONE)) {
		if (callback == NULL) return Void();
		Return<void> ret =
		callback->setCurrentUsbFunctionsCb(functions, Status::SUCCESS);
		if (!ret.isOk())
		ALOGE("Error while calling setCurrentUsbFunctionsCb %s",
			ret.description().c_str());
		return Void();
	}

	status = validateAndSetVidPid(functions);
	if (status != Status::SUCCESS) {
		goto error;
	}

	status = setupFunctions(functions, callback, timeout);
	if (status != Status::SUCCESS) {
		goto error;
	}
	ALOGD("Usb Gadget setcurrent functions called successfully");
	return Void();

error:
	ALOGD("Usb Gadget setcurrent functions failed");
	if (callback == NULL) return Void();
		Return<void> ret = callback->setCurrentUsbFunctionsCb(functions, status);
	if (!ret.isOk())
		ALOGE("Error while calling setCurrentUsbFunctionsCb %s",
			ret.description().c_str());
	return Void();
}
}  // namespace implementation
}  // namespace V1_0
}  // namespace gadget
}  // namespace usb
}  // namespace hardware
}  // namespace android
