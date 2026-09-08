/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include <string>
#include <vector>

#define LOG_TAG "GrubBootControlUtil"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <libgrub_editenv.h>

#include <GrubBootControl.h>

using namespace libgrub_editenv;

using android::base::unique_fd;
using std::lock_guard;
using std::mutex;
using std::string;
using std::to_string;
using std::vector;

namespace libgrub_boot_control {

namespace {

const int INVALID_SLOT = -1;
const int COMMAND_FAILED = -2;

}  // namespace

GrubBootControl::GrubBootControl(string grubenv_path, vector<string> slots, string var_key_prefix)
    : mSlots(slots), mVarKeyPrefix(var_key_prefix) {
    CHECK(!mSlots.empty()) << "No slot";

    mFd = unique_fd(TEMP_FAILURE_RETRY(open(grubenv_path.c_str(), O_RDWR)));
    if (mFd.ok()) {
        CHECK(LoadFdToMap(mFd, &mMap)) << "Failed to parse " << grubenv_path;
    } else {
        mFd = unique_fd(TEMP_FAILURE_RETRY(open(grubenv_path.c_str(), O_WRONLY | O_CREAT, 0644)));
        CHECK(mFd.ok()) << "Failed to open or create " << grubenv_path;
        InitGrubVars();
        CHECK(CommitGrubVars());
    }
}

GrubBootControl::~GrubBootControl() {
    CommitGrubVars();
}

bool GrubBootControl::CommitGrubVars() {
    lock_guard<mutex> lock(mMapMutex);
    return CommitGrubVarsLocked();
}

bool GrubBootControl::CommitGrubVarsLocked() {
    if (lseek(mFd, 0, SEEK_SET) == -1) LOG(FATAL) << "lseek error";

    if (TEMP_FAILURE_RETRY(WriteFdFromMap(mFd, mMap))) return true;

    // The environment block is likely full because of variables that are not
    // ours, drop them and try again
    RemoveUnusedElementsFromMapLocked();
    if (TEMP_FAILURE_RETRY(WriteFdFromMap(mFd, mMap))) return true;

    LOG(ERROR) << "Failed to commit grub vars";
    return false;
}

void GrubBootControl::PrintGrubVars() {
    lock_guard<mutex> lock(mMapMutex);
    LOG(DEBUG) << "Print GRUB variables:";
    for (const auto& [key, value] : mMap) {
        LOG(DEBUG) << key << "=" << value;
    }
}

bool GrubBootControl::IsValidSlot(int slot) {
    if (slot < 0 || slot > getNumberSlots() - 1) {
        LOG(WARNING) << "Invalid slot: " << to_string(slot);
        return false;
    }
    return true;
}

string GrubBootControl::GetItemValueLocked(const string& key) {
    const auto it = mMap.find(key);
    return (it == mMap.end()) ? string() : it->second;
}

void GrubBootControl::SetItemValueLocked(const string& key, const string& value) {
    mMap.insert_or_assign(key, value);
}

string GrubBootControl::GetItemKeyForGlobal(string item) {
    return mVarKeyPrefix + "global_" + item;
}

string GrubBootControl::GetItemValueForGlobal(string item) {
    const string key = GetItemKeyForGlobal(item);
    lock_guard<mutex> lock(mMapMutex);
    return GetItemValueLocked(key);
}

bool GrubBootControl::SetItemValueForGlobal(string item, string value, bool commit) {
    const string key = GetItemKeyForGlobal(item);
    lock_guard<mutex> lock(mMapMutex);
    SetItemValueLocked(key, value);
    return commit ? CommitGrubVarsLocked() : true;
}

string GrubBootControl::GetItemKeyForSlot(int slot, string item) {
    string slot_str;
    if (IsValidSlot(slot)) {
        slot_str = GetStringFromSlotNumber(slot);
    } else {
        slot_str = "UNKNOWN";
    }
    return mVarKeyPrefix + "slot_" + slot_str + "_" + item;
}

string GrubBootControl::GetItemValueForSlot(int slot, string item) {
    const string key = GetItemKeyForSlot(slot, item);
    lock_guard<mutex> lock(mMapMutex);
    return GetItemValueLocked(key);
}

bool GrubBootControl::SetItemValueForSlot(int slot, string item, string value, bool commit) {
    const string key = GetItemKeyForSlot(slot, item);
    lock_guard<mutex> lock(mMapMutex);
    SetItemValueLocked(key, value);
    return commit ? CommitGrubVarsLocked() : true;
}

bool GrubBootControl::SetItemValueForAllSlots(string item, string value, bool commit) {
    lock_guard<mutex> lock(mMapMutex);
    for (int i = 0; i < getNumberSlots(); i++) {
        SetItemValueLocked(GetItemKeyForSlot(i, item), value);
    }
    return commit ? CommitGrubVarsLocked() : true;
}

string GrubBootControl::GetStringFromSlotNumber(int slot) {
    return mSlots[slot];
}

int GrubBootControl::GetSlotNumberFromString(string str) {
    for (int i = 0; i < getNumberSlots(); i++) {
        if (GetStringFromSlotNumber(i) == str) {
            return i;
        }
    }
    return INVALID_SLOT;
}

}  // namespace libgrub_boot_control
