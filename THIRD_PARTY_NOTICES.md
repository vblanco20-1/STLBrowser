# Third-party notices

The vendored dependencies were copied from `E:/ProgrammingProjects/vulkan-guide-2` at reference revision `500d885a875c00bed1ec6d3ac6a1433563ee09c6`. No reference build outputs or absolute runtime paths are required.

| Component | Observed version / provenance | Notice |
|---|---|---|
| Vulkan Guide setup reference | Revision above | `licenses/VulkanGuide.txt` |
| Dear ImGui and SDL2/Vulkan backends | 1.90.6 WIP, copyright 2014–2024 Omar Cornut | `licenses/ImGui.txt` (upstream 1.90.5 MIT notice, same copyright period) |
| SDL2 | 2.28.4 | `licenses/SDL.txt` |
| Vulkan Memory Allocator | 3.1.0-development | `licenses/VMA.txt`, also embedded in its header |
| vk-bootstrap | Reference's vendored snapshot | `licenses/vk-bootstrap.txt`, also embedded in its sources |
| GLM | 0.9.9.7, used under the MIT option | `licenses/GLM.txt` |
| fmt | Reference's vendored copy, currently unused by application targets | `third_party/fmt/LICENSE.rst` |

ImGui and GLM notices were retrieved from their respective upstream version tags because the reference's dependency copies omitted standalone license files. Other notices were copied or extracted from the reference sources. The installed application includes all notices for linked components. Windows Segoe UI is loaded from the operating system and is not bundled.
