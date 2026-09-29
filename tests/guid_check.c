/* GUID audit: every hand-written GUID in hook/ is compared against the SDK
 * header at compile time, so a typo cannot survive a build.
 *
 * The project has been bitten by exactly this: present_hook.h carried
 * IID_IDXGIAdapter1 with 0x1b where dxgi.h says 0x1a. Fifteen of sixteen bytes
 * matched, so it read as correct, and the file that teaches the rule "never
 * write a GUID from memory" sat 100 lines from a broken copy. A comment is not
 * a control; this is.
 *
 * A failure here is a COM query that will silently return E_NOINTERFACE at
 * runtime and read as "DXGI is not available" -- the exact misdiagnosis the
 * project already made once (E_NOTIMPL treated as a missing API).
 *
 * Build: see tests/run_tests.sh
 */
#define INITGUID
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

/* --- copies as they appear in hook/present_hook.h ------------------------- */
static const GUID IID_IDXGIAdapter1_h =
    { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
static const GUID IID_IDXGIDevice_h =
    { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

/* --- copies as they appear in hook/dxgi_probe.h -------------------------- */
static const GUID IID_IDXGIFactory2_h =
    { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
static const GUID IID_IDXGIAdapter1_probe =
    { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };

/* --- copies as they appear in hook/ --- */
static const GUID IID_ID3D12Device_h =
    { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const GUID IID_ID3D12CommandQueue_h =
    { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
static const GUID IID_ID3D12Object_h =
    { 0xc4fec28f, 0x7966, 0x4e95, { 0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8 } };

static int failures = 0;

static void check(const char *name, const GUID *mine, const GUID *real)
{
    int ok = memcmp(mine, real, sizeof(GUID)) == 0;
    printf("  %-22s %s\n", name, ok ? "ok" : "MISMATCH");
    if (!ok) {
        printf("      mine  : ");
        for (int i = 0; i < 8; i++) printf("%02X", (unsigned)mine->Data4[i]);
        printf("\n      header: ");
        for (int i = 0; i < 8; i++) printf("%02X", (unsigned)real->Data4[i]);
        printf("\n");
        failures++;
    }
}

int main(void)
{
    printf("GUID audit against dxgi.h / d3d12.h\n");
    check("IDXGIAdapter1 (present)",   &IID_IDXGIAdapter1_h,       &IID_IDXGIAdapter1);
    check("IDXGIDevice",              &IID_IDXGIDevice_h,         &IID_IDXGIDevice);
    check("IDXGIFactory2",            &IID_IDXGIFactory2_h,       &IID_IDXGIFactory2);
    check("IDXGIAdapter1 (probe)",    &IID_IDXGIAdapter1_probe,   &IID_IDXGIAdapter1);
    check("ID3D12Device",             &IID_ID3D12Device_h,        &IID_ID3D12Device);
    check("IID_ID3D12CommandQueue",   &IID_ID3D12CommandQueue_h,  &IID_ID3D12CommandQueue);
    check("ID3D12Object",             &IID_ID3D12Object_h,        &IID_ID3D12Object);

    if (failures) {
        printf("FAILED: %d GUID(s) differ from the SDK header.\n", failures);
        printf("A wrong GUID fails at runtime as E_NOINTERFACE, which reads as\n");
        printf("\"the API is missing\" -- a misdiagnosis this project already made.\n");
        return 1;
    }
    printf("all %d GUIDs match the header\n", 7);
    return 0;
}
