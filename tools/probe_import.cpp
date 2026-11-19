// tools/probe_import.cpp — what can this machine's GPU import, as mayag sees it?
#include <mayag/mayag.hpp>
#include <mayag/backend/vulkan.hpp>

#include <cstdio>

int main() {
    mayag::backend::VulkanDevice d;
    if (!d.init_offscreen()) {
        std::puts("vulkan: no device");
        return 1;
    }
    std::printf("can_import: %d\n", d.can_import());
    const auto caps = d.sync_caps();
    std::printf("timeline: %d  import_syncobj: %d\n", caps.timeline, caps.import_syncobj);
    for (const auto& f : d.importable_formats())
        std::printf("  fourcc %.4s  modifier 0x%016llx\n", reinterpret_cast<const char*>(&f.format),
                    static_cast<unsigned long long>(f.modifier));
    d.destroy();
}
