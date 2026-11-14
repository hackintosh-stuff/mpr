#include "MPRUserClient.h"
#include "MPRController.h"
#include "MPRControlABI.h"
#include <kern/task.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <string.h>
#define super IOUserClient
OSDefineMetaClassAndStructors(MPRUserClient, IOUserClient)

bool MPRUserClient::initWithTask(task_t task, void *securityId, UInt32 type,
                                OSDictionary *properties)
{
    fController = NULL;
    fCallLock = NULL;
    if (type != kMPRControlConnectType ||
        clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess ||
        !super::initWithTask(task, securityId, type, properties)) return false;
    fCallLock = IOLockAlloc();
    return fCallLock != NULL;
}
bool MPRUserClient::start(IOService *provider)
{
    MPRController *controller = OSDynamicCast(MPRController, provider);
    if (!controller || !super::start(provider)) return false;
    controller->retain();
    fController = controller;
    return true;
}
void MPRUserClient::stop(IOService *provider)
{
    MPRController *controller = NULL;
    if (fCallLock) {
        IOLockLock(fCallLock);
        controller = fController;
        fController = NULL;
        IOLockUnlock(fCallLock);
    }
    if (controller) controller->release();
    super::stop(provider);
}
void MPRUserClient::free(void)
{
    if (fCallLock) { IOLockFree(fCallLock); fCallLock = NULL; }
    super::free();
}
IOReturn MPRUserClient::clientClose(void)
{
    terminate();
    return kIOReturnSuccess;
}
IOReturn MPRUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    /* Recheck the invoking task: possession of a connection inherited or
     * transferred from a privileged process is not authorization. */
    if (clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (selector || !a || a->scalarInputCount || a->scalarOutputCount ||
        a->asyncWakePort || a->asyncReferenceCount || !fCallLock) return kIOReturnBadArgument;
    const UInt64 inputSize = a->structureInputDescriptor
        ? a->structureInputDescriptor->getLength() : a->structureInputSize;
    const UInt64 outputSize = a->structureOutputDescriptor
        ? a->structureOutputDescriptor->getLength() : a->structureOutputSize;
    if (inputSize < sizeof(MPRControlInput) ||
        inputSize > sizeof(MPRControlInput) + kMPRControlMaxRequest + uint64_t(kMPRControlMaxData) ||
        outputSize < sizeof(MPRControlOutput) ||
        outputSize > sizeof(MPRControlOutput) + uint64_t(kMPRControlMaxData) ||
        (!a->structureInputDescriptor && !a->structureInput) ||
        (!a->structureOutputDescriptor && !a->structureOutput)) return kIOReturnBadArgument;
    UInt8 *input = (UInt8 *)IOMalloc((vm_size_t)inputSize);
    UInt8 *output = (UInt8 *)IOMalloc((vm_size_t)outputSize);
    if (!input || !output) {
        if (input) IOFree(input, (vm_size_t)inputSize);
        if (output) IOFree(output, (vm_size_t)outputSize);
        return kIOReturnNoMemory;
    }
    bzero(output, (size_t)outputSize);
    /* Large structure arguments arrive as IOMemoryDescriptors. readBytes and
     * writeBytes require a prepared (wired) descriptor; using an unprepared
     * one panics in IOGeneralMemoryDescriptor::getPhysicalSegment. */
    IOMemoryDescriptor *inDesc = a->structureInputDescriptor;
    IOMemoryDescriptor *outDesc = a->structureOutputDescriptor;
    IOReturn inReady = inDesc ? inDesc->prepare(kIODirectionOut) : kIOReturnSuccess;
    IOReturn outReady = outDesc ? outDesc->prepare(kIODirectionIn) : kIOReturnSuccess;
    if (inReady != kIOReturnSuccess || outReady != kIOReturnSuccess) {
        if (inReady == kIOReturnSuccess && inDesc) inDesc->complete(kIODirectionOut);
        if (outReady == kIOReturnSuccess && outDesc) outDesc->complete(kIODirectionIn);
        IOFree(output, (vm_size_t)outputSize);
        IOFree(input, (vm_size_t)inputSize);
        return kIOReturnVMError;
    }
    IOReturn result = kIOReturnSuccess;
    if (inDesc) {
        if (inDesc->readBytes(0, input, (IOByteCount)inputSize) != inputSize)
            result = kIOReturnVMError;
    } else memcpy(input, a->structureInput, (size_t)inputSize);
    if (result == kIOReturnSuccess &&
        !mprControlInputValid(*(const MPRControlInput *)input, inputSize, outputSize))
        result = kIOReturnBadArgument;
    UInt32 produced = sizeof(MPRControlOutput);
    if (result == kIOReturnSuccess) {
        IOLockLock(fCallLock);
        if (!fController || isInactive()) result = kIOReturnNotAttached;
        else result = fController->controlRequest(input, (UInt32)inputSize,
            output, (UInt32)outputSize, &produced);
        IOLockUnlock(fCallLock);
    }
    if (result == kIOReturnSuccess && produced <= outputSize) {
        if (outDesc) {
            if (outDesc->writeBytes(0, output, produced) != produced)
                result = kIOReturnVMError;
            a->structureOutputDescriptorSize = produced;
        } else {
            memcpy(a->structureOutput, output, produced);
            a->structureOutputSize = produced;
        }
    } else if (result == kIOReturnSuccess) result = kIOReturnOverrun;
    if (outDesc) outDesc->complete(kIODirectionIn);
    if (inDesc) inDesc->complete(kIODirectionOut);
    IOFree(output, (vm_size_t)outputSize);
    IOFree(input, (vm_size_t)inputSize);
    return result;
}

IOReturn MPRController::newUserClient(task_t task, void *securityId, UInt32 type,
                                      OSDictionary *properties, IOUserClient **handler)
{
    if (!handler) return kIOReturnBadArgument;
    if (!managementEnabled() || isInactive()) return kIOReturnNotPermitted;
    *handler = NULL;
    if (type != kMPRControlConnectType ||
        IOUserClient::clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    MPRUserClient *client = new MPRUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, securityId, type, properties)) {
        client->release(); return kIOReturnNotPrivileged;
    }
    if (!client->attach(this)) { client->release(); return kIOReturnNotAttached; }
    if (!client->start(this)) {
        client->detach(this); client->release(); return kIOReturnNotAttached;
    }
    *handler = client;
    return kIOReturnSuccess;
}
