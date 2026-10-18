using System.Runtime.InteropServices;
using System.Text;

namespace Hajinmon.Launcher;

/// <summary>
/// Lists the Vulkan physical devices in enumeration order, which is the index
/// the game's vulkan_device option expects.
/// </summary>
public static class VulkanGpuList
{
    [StructLayout(LayoutKind.Sequential)]
    private struct VkInstanceCreateInfo
    {
        public int sType;
        public IntPtr pNext;
        public uint flags;
        public IntPtr pApplicationInfo;
        public uint enabledLayerCount;
        public IntPtr ppEnabledLayerNames;
        public uint enabledExtensionCount;
        public IntPtr ppEnabledExtensionNames;
    }

    private const int VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1;
    private const int VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2;
    private const int VK_PHYSICAL_DEVICE_TYPE_CPU = 4;

    // VkPhysicalDeviceProperties: deviceType at 16, deviceName (char[256]) at 20.
    private const int PropertiesSize = 1024;
    private const int DeviceTypeOffset = 16;
    private const int DeviceNameOffset = 20;

    [DllImport("vulkan-1.dll")]
    private static extern int vkCreateInstance(ref VkInstanceCreateInfo info, IntPtr allocator, out IntPtr instance);

    [DllImport("vulkan-1.dll")]
    private static extern void vkDestroyInstance(IntPtr instance, IntPtr allocator);

    [DllImport("vulkan-1.dll")]
    private static extern int vkEnumeratePhysicalDevices(IntPtr instance, ref uint count, IntPtr[]? devices);

    [DllImport("vulkan-1.dll")]
    private static extern void vkGetPhysicalDeviceProperties(IntPtr device, byte[] properties);

    public sealed record Device(int Index, string Name, bool Dedicated);

    /// <summary>The devices, or an empty list when Vulkan is not available.</summary>
    public static List<Device> Enumerate()
    {
        var result = new List<Device>();
        IntPtr instance = IntPtr.Zero;
        try
        {
            var info = new VkInstanceCreateInfo { sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            if (vkCreateInstance(ref info, IntPtr.Zero, out instance) != 0) return result;
            uint count = 0;
            if (vkEnumeratePhysicalDevices(instance, ref count, null) != 0 || count == 0) return result;
            var devices = new IntPtr[count];
            if (vkEnumeratePhysicalDevices(instance, ref count, devices) != 0) return result;
            var properties = new byte[PropertiesSize];
            for (int i = 0; i < count; i++)
            {
                Array.Clear(properties);
                vkGetPhysicalDeviceProperties(devices[i], properties);
                int type = BitConverter.ToInt32(properties, DeviceTypeOffset);
                if (type == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;  // Software renderers.
                int end = Array.IndexOf(properties, (byte)0, DeviceNameOffset, 256);
                string name = Encoding.UTF8.GetString(properties, DeviceNameOffset,
                                                      (end < 0 ? DeviceNameOffset + 256 : end) - DeviceNameOffset);
                result.Add(new Device(i, name, type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
            }
        }
        catch (Exception)
        {
            // No Vulkan driver (vulkan-1.dll missing): the list stays empty.
        }
        finally
        {
            if (instance != IntPtr.Zero) vkDestroyInstance(instance, IntPtr.Zero);
        }
        return result;
    }
}
