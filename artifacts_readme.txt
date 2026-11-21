Remix Plus build
================

Remix Plus is a community fork of NVIDIA RTX Remix (dxvk-remix). It is not
affiliated with, endorsed by or supported by NVIDIA.

Installation:

1. Pick the package for your game. Most D3D9 games are 32-bit and need the
   `rtx-remix-for-x86-games-...` package, which includes the bridge. Use the
   `rtx-remix-for-x64-games-...` package only for a 64-bit game.

2. Copy every file in the package into the game's folder, next to the game's
   executable. Copy all of them: the runtime loads many DLLs that ship beside
   d3d9.dll, and a partial copy fails to load.

3. Start the game and press Alt+X to open the Remix menu.

Notes:

- You should be prompted to overwrite existing files. If you aren't, you're
  probably putting the files in the wrong place.
- Logs are in the game's rtx-remix/logs folder. Attach remix-dxvk.log (and
  bridge32.log and bridge64.log for a 32-bit game) to bug reports, and after
  a crash the newest .dmp file from rtx-remix/logs/.sentry-native/reports.
- Bugs go to https://github.com/RemixProjGroup/dxvk-remix/issues, not to NVIDIA.
- The d3d11-forwarders folder of the x64 package holds d3d11.dll and
  dxgi.dll for Remix API plugins that render a D3D11 game. Copy them next to
  d3d9.dll only when a plugin's instructions say so. With them in its folder,
  any D3D11 game runs its D3D11 calls through Remix, without path tracing.

Licenses and credits:

This build contains code under the zlib license (DXVK), the MIT license
(NVIDIA dxvk-remix and Remix Plus), and the licenses of the bundled libraries
and SDKs. LICENSE, LICENSE-MIT, CREDITS.md and ThirdPartyLicenses.txt list
them in full. Release packages include those files. They are also in the
source tree at https://github.com/RemixProjGroup/dxvk-remix.
