using System.Runtime.InteropServices;

namespace Hajinmon.Launcher;

/// <summary>
/// Lists the DXGI adapters in enumeration order, which is the index the game's
/// d3d12_adapter option expects.
/// </summary>
public static class GpuList
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DXGI_ADAPTER_DESC1
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId, DeviceId, SubSysId, Revision;
        public nuint DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
        public uint AdapterLuidLow;
        public int AdapterLuidHigh;
        public uint Flags;
    }

    private const uint DXGI_ADAPTER_FLAG_SOFTWARE = 2;

    [ComImport, Guid("770aae78-f26f-4dba-a829-253c83d1b387"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDXGIFactory1
    {
        // IDXGIObject
        void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
        // IDXGIFactory
        void EnumAdapters(); void MakeWindowAssociation(); void GetWindowAssociation();
        void CreateSwapChain(); void CreateSoftwareAdapter();
        // IDXGIFactory1
        [PreserveSig] int EnumAdapters1(uint index, out IDXGIAdapter1 adapter);
        [PreserveSig] bool IsCurrent();
    }

    [ComImport, Guid("29038f61-3839-4626-91fd-086879011a05"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDXGIAdapter1
    {
        // IDXGIObject
        void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
        // IDXGIAdapter
        void EnumOutputs(); void GetDesc(); void CheckInterfaceSupport();
        // IDXGIAdapter1
        [PreserveSig] int GetDesc1(out DXGI_ADAPTER_DESC1 desc);
    }

    [DllImport("dxgi.dll")]
    private static extern int CreateDXGIFactory1(ref Guid riid, out IDXGIFactory1 factory);

    public sealed record Adapter(int Index, string Name, ulong VideoMemory);

    public static List<Adapter> Enumerate()
    {
        var result = new List<Adapter>();
        try
        {
            Guid iid = typeof(IDXGIFactory1).GUID;
            if (CreateDXGIFactory1(ref iid, out IDXGIFactory1 factory) != 0) return result;
            for (uint i = 0; factory.EnumAdapters1(i, out IDXGIAdapter1 adapter) == 0; i++)
            {
                if (adapter.GetDesc1(out DXGI_ADAPTER_DESC1 desc) == 0 &&
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
                {
                    result.Add(new Adapter((int)i, desc.Description.Trim(), desc.DedicatedVideoMemory));
                }
                Marshal.ReleaseComObject(adapter);
            }
            Marshal.ReleaseComObject(factory);
        }
        catch (Exception)
        {
            // Without DXGI the list just stays empty ("Automatic" is still available).
        }
        return result;
    }
}
