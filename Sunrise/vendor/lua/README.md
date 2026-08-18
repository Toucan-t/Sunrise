# Lua dependency

Sunrise's activity-session scripting runtime uses the native `lua` NuGet package, pinned to Lua
5.4.8. The package is restored under `vendor/lua/packages` by the repository `NuGet.Config` and is
linked statically into the existing shim DLL.

The Lua package is a build dependency only. Project-authored scripts are deliberately separate and
live under `Sunrise/scripts`; the build copies those editable files beside the output DLL in a
sibling `scripts` directory.
