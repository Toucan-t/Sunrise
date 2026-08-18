# Third-party software

Sunrise uses the following reviewed upstream dependencies:

- Microsoft Detours 4.0.1. The reviewed source and license are retained under
  `Sunrise/vendor/detours`.
- Dear ImGui 1.92.6. The upstream MIT notice is retained at
  `Sunrise/vendor/imgui/LICENSE.txt` and embedded in the DLL as a resource.
- Lua 5.4.8. The native NuGet package is restored under `Sunrise/vendor/lua/packages` and
  linked statically into the shim DLL. Project-authored Lua scripts remain external files.

Project-owned Sunrise source follows the project coding rules. Vendored upstream source/build
inputs are kept isolated and are not rewritten by the project formatter.
