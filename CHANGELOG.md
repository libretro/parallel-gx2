# Changelog of the libretro core

What changed in the libretro core of this fork, newest first. Changes that come with a merge of upstream Cemu are in upstream's own history; a merge is noted here with the upstream version it brought in.

## Unreleased

- Side by Side, Top Bottom and Both Screens: the GamePad screen no longer flashes black. Titles that scan out the GamePad less often than the TV (Wind Waker HD) left it black in every frame with only a TV picture, about twice a second; each screen now keeps its last picture until the title sends a new one, as a GamePad does. A frame also no longer blits the TV picture a second time when the GamePad's arrives after it, and on Vulkan the mouse cursor costs a copy only where the next frame leaves it uncovered.
- New screen layout Both Screens (Controller > Layout 1-5): the TV and the GamePad at their own sizes, the GamePad centred under the TV, or above it with GamePad Position Swapped. Unlike Top Bottom, neither picture is stretched. Touches count on the GamePad's part, on Vulkan and OpenGL. A frontend with video views (RetroArch's, libretro/RetroArch#19811) gets the two pictures as two screens and lays them out itself: one under the other or side by side, the GamePad in a window or on a display of its own, both on a headset, each with its own shader. The single-screen layouts go to it as one screen. Side by Side, Top Bottom and Picture in Picture look as before, except in the frontend's stereo modes and on a headset, where they become Both Screens.
- The arm64 cores (Linux, Android arm64-v8a, macOS, iOS and tvOS) and Android x86_64 build again with the Makefile. Since the static Makefile their links stopped: the x64 recompiler was built into arm64 cores next to the AArch64 one, and both define the same function; H.264 decoding brought in a selector for 32-bit ARM code that is not built; the crash handler's ELF symbol table, which Android cores link against though they never run it, was only in the Linux sources; and Apple arm64 linked the H.264 decoder's symbol aliases twice. Each recompiler backend is now built only for its own architecture.
- zlib and Zstandard are libretro-common's, deflate and rzstd, and the zlib and zstd submodules are gone from the tree: a checkout without them no longer stops at the graphic pack tool wanting zlib's sources. The shader and pipeline caches' entries are still Zstandard frames made with Cemu's dictionaries, so a cache goes between the core and standalone Cemu either way, as before. The console's own zlib (zlib125), which games call, runs on libretro-common's deflate too; its deflate() takes Z_PARTIAL_FLUSH, Z_SYNC_FLUSH and Z_FULL_FLUSH as no flush, the output following at the next block or at Z_FINISH, and logs the first time a game asks for one.
- Wii Remotes and the Pro and Classic Controllers are read without locks. Every pad read a game made took the input manager's reader lock, then each emulated controller's for every button and stick it looked at, each controller's settings mutex and the spinlock around its pressed buttons - while retro_run's thread took the same locks to write the frame's input. Which controllers there are and how they are mapped are now published copy-on-write and read with no lock; each controller's state and settings are published whole each time they change, through a seqlock no reader waits on; and its pressed buttons are a bitset instead of a list searched under a spinlock. That also ends the stick and trigger values being read while retro_run was writing them.
- Online (Pretendo): the nex thread no longer wakes a thousand times a second while services are connected, or five times a second while none are. It sleeps in `select()` on its services' sockets and a loopback wake socket, so a packet, a queued request or a newly registered service ends the wait at once; the protocol's own clocks (retries, pings, request timeouts, all a second or more) are looked at every 100 ms. The logins do the same, and registering for secure access no longer spins a host core at 100% for the length of a round trip. Its request and service queues take no lock.
- Vulkan: the shader compile pool takes no lock. Each queued shader is held in the queue by a ticket that whoever compiles it - a compile thread, the GPU thread when it needs the shader at once, or the shader's own destructor if it was never compiled - claims with one exchange, so nothing is taken out of the middle of the queue under a mutex any more; the compile state is atomic, and waiting for a compile sleeps on a shared wake. A shader destroyed while still queued no longer leaves the compile threads a pointer to freed memory, and one destroyed while being compiled waits for it.
- The worker queues no longer take a mutex for every item. GX2's event callbacks (several every frame), the thread deallocators, curl's workers and the pipeline cache writer each have one consumer, and push through a lock-free list it takes from in order; the two pipeline compile pools, with several consumers, use a lock-free ring with an overflow list past 1024 queued. A worker with nothing to do sleeps on an eventcount instead of a condition variable.
- Vulkan: loading a title's pipeline cache no longer serialises its compile threads on spinlocks. Every pipeline they built took one spinlock shared by all shaders to register with its shaders, another shared by all framebuffers, one around its `new` and `delete`, and one around the set of cached hashes; the shaders' and framebuffers' pipeline lists are now lock-free, the allocations need no lock, and the hashes go to the GPU thread through a lock-free list. The renderer's destruction queue, filled from the GPU thread and the compile threads, is lock-free on both sides too.
- The two locks the emulated CPU hit hardest are gone, both spinlocks every core took. Every guest time read (OSGetTime, the scheduler's alarms, mftb) now does its arithmetic on an epoch it reads without a lock, and only a refined TSC rate (or a changed timer speed) starts a new one, at the time the old one gives, so no time already counted moves. Every data cache flush the CPU reports to the GPU, down to each dcbf/dcbst the interpreter runs, sets a bit in a lock-free page set instead of a spinlocked hash of vectors searched linearly, and a page already reported costs three loads.
- AX no longer takes a lock every 3 ms to reach the audio output. The reader-writer lock around the audio devices is replaced by a gate that whatever plays through them passes with one atomic count, never waiting; only a title starting or stopping (or a Skylander portal coming up, which now goes through the same gate instead of none) waits for the players already in.
- Vulkan: retro_run no longer takes a lock every frame that could hold the frontend's thread behind a GPU submit. It handed the frontend's Vulkan interface over again each frame under an exclusive lock, which waited for any submit in flight - itself waiting for the frontend's queue lock. Handing over the same interface now does nothing, and only a changed one waits for the submits that took the queue lock through the old one. Submits no longer take a reader lock either.
- GamePad motion and rumble are handed between retro_run and the game's threads without a lock: the motion sample through a seqlock neither side waits on, the rumble requests through a lock-free list drained once a frame.
- log.txt is written without a lock. Every logged line used to take a mutex and write and flush the file while holding it, so the GPU thread and the emulated CPU's threads queued on each other whenever they logged - and a thread that crashed while it held the mutex left the crash handler waiting for it for good, with no crash log. Each line now reserves its place in the file and is written there directly, still unbuffered, so a crash report keeps its last lines as before. The local time in each line's stamp is also no longer read through the non-thread-safe `localtime`. Switching log.txt off waits only for writes already under way before it closes the file.
- The title scan that loading a title waits for on a cold cache, and Install Games' rescan, no longer finish up to a tenth of a second late: the wait sleeps until the scan says it is done instead of checking every 100 ms. The same for SpotPass (boss): a game waiting on a task no longer waits a tenth of a second at the least, and the daemon no longer wakes ten times a second with nothing to do. Closing content no longer waits on millisecond polls for the IOSU workers and the SpotPass daemon to leave, nor eShop (nim) on half-second ones for the accounts.
- Audio Thread: with nothing buffered, the pull from RetroArch's audio thread slept a fixed millisecond, so audio that arrived just after it went out up to a millisecond late. It now waits only until AX's next block arrives (a millisecond at most, as before), and a frontend stopping its audio thread no longer waits that millisecond out.
- Closing content, or losing the graphics context, on Vulkan: the waits for the GPU thread to reach the pause gate, park there and hand the context back end the moment it does, rather than up to a millisecond later on each of them.
- Vulkan: the GPU thread no longer keeps a CPU core busy while it waits for the game's next commands. Upstream Cemu spins there, for as long as the emulated CPU takes, which on a four-core phone is a core the emulated CPU's own threads compete with, and power and heat on any device; it now sleeps until the commands are submitted, after the short spin that keeps a command that follows closely as quick to pick up as before. A game copying a surface no longer spins either, while the GPU thread does the copy.
- Loading a title no longer waits on a series of clocks: the GPU thread's start, the title starting, the graphic packs, the GPU's init and the IOSU services' threads were each checked every 1 to 50 ms, and the services alone cost 10 ms apiece. Each wait now ends the moment what it waits for has happened. Vulkan: the GPU thread no longer wakes a thousand times a second while a title waits to call GX2Init, nor sixty times a second before it starts, and a title stopped before it got that far no longer keeps the GPU thread waiting for it.
- Vulkan compiles its shaders with libretro-common's rslc in place of glslang, which is gone from the tree. The graphic packs' pixel shaders draw what they drew with glslang, pixel for pixel (checked on Mesa's lavapipe). The game's shaders now ask for 32-bit round-to-even arithmetic where the GPU offers it, as the decompiler always meant them to: glslang kept only the last of the 16, 32 and 64-bit requests, so none that applied.
- Wii U archives (`.wua`) are read and written with libretro-common's zar in place of the ZArchive library, which is gone from the tree. An archive's index is read into memory once when it is opened, so finding a file or listing a folder no longer touches the disk and no longer waits on another thread, and each open file reads through a reader of its own instead of every read of the archive queueing behind one lock. Convert to WUA writes archives standalone Cemu reads, and stops with an error when the destination cannot be written, where it used to carry on.
- The emulated controllers are updated once a frame, right after RetroArch's input is read, instead of by a thread of their own waking a thousand times a second to update them between frames, when nothing had changed.
- Vulkan goes through the frontend alone too: the core's own instance and device setup, its X11, XCB, Wayland, Win32 and Metal surfaces, its swapchain and the Breath of the Wild child-process workaround that came with them are gone - none of it ever ran under libretro, where the device, surface and swapchain are RetroArch's. Linux builds no longer need X11 headers, or any headers beyond the compiler's.
- Boot no longer waits up to three seconds for the CPU timer's calibration: a first estimate is ready 25 ms after the core loads and is refined in the background.
- The recompiler starts on a newly run function at once instead of at its next 10 ms look, so hot code spends less time interpreted, and its work queue no longer allocates.
- Vulkan: loading the pipeline cache no longer pauses 10 ms whenever 50 pipelines are queued, which held a warm cache to 5,000 pipelines a second; closing content no longer waits up to 250 ms for the pipeline cache's save thread.
- Audio goes to RetroArch straight out of the ring buffer, and AX's channels are converted straight into it: no copies on the way, and no 192 KiB flush buffer.
- The frame gate and the frame handoff between the GPU thread and `retro_run` take no locks.
- OpenGL goes through the frontend alone: the core no longer makes GL contexts of its own (WGL on Windows, GLX on X11, EGL on Wayland), links no GL, EGL or X11 library, and loads every GL function from RetroArch. Cemu's GPU thread runs as a cothread on RetroArch's thread, where its context is current, and the finished frame is blitted into RetroArch's framebuffer instead of read back into memory and uploaded again each frame. Wayland no longer has an OpenGL path of its own.
- Android: the fibers the emulated cores run on keep libco's state per thread (`LIBCO_MP`); without it a switch on one core could save its registers into another core's fiber.
- The core is now ParaLLEl GX2, `parallel_gx2_libretro`. RetroArch keeps core options, remaps and overrides under the core's name, so they start from the defaults: to keep them, copy RetroArch's `config/Cemu/` to `config/ParaLLEl GX2/` and rename the files in it from `Cemu.*` to `ParaLLEl GX2.*`. The core's own files stay in `<system>/Cemu/` and `<save>/Cemu/`.
- Online play (Nintendo Network, Pretendo, SpotPass, Miiverse) goes through the HTTP and TLS client RetroArch's libretro-common has, in place of libcurl and OpenSSL: the client certificates and the RSA/AES-256-CBC suite the console's services ask for included.
- Linux: content loaded a second time in the same RetroArch, after closing it, no longer crashes RetroArch. The core's OpenSSL read the system's `openssl.cnf`, which loads the pkcs11 engine on many distributions; that system library stays loaded when the core is unloaded and kept pointers into the first copy of the core, so the second load crashed in it. The core no longer has OpenSSL at all.
- A crash is no longer the core's to end: Cemu's crash handlers still write their crash log to `log.txt`, and then hand the crash to the handler there was before, where they used to end RetroArch at once (`_Exit` on Linux and Android, `exit(0)` on Windows), so the system's crash report (Android's tombstone, Windows Error Reporting) is written and names the fault. Ctrl+C and SIGTERM are left to RetroArch, which the core used to end on them (sco8487).
- New options Controller > Stick Dead Zone (Player 1) and (Players 2-4), 0% to 50%, 25% by default as standalone Cemu. The GamePad's sticks had no dead zone at all, so the slightest touch, or a stick resting a hair off centre, moved the character, while a Pro Controller had Cemu's 25% (demifiend9, Smash 4); now both follow the option, and sticks that rest at the centre can go lower (NNshi).
- Merged up to upstream Cemu 2.6-372 (29f2f1fa): coreinit alarm fixes and the alarm queue functions, recompiler entry speed-up and a function-scanning fix, the GamePad (DRC) audio mix fix, a GX2 flush fix and graphic pack patch-group fixes. The core reports upstream's version.
- GamePad touch screen: a tap or click lands where it is made. The core handed games the position in screen pixels where they expect the touch panel's raw values, as standalone Cemu sends them, so every touch landed in the lower-left corner (NNshi). With a mouse, an arrow cursor now shows over the GamePad picture while it moves, and hides after 3 s without movement or a tap elsewhere; Vulkan and OpenGL (sco8487).
- With the GamePad Screen layout, the picture is handed to RetroArch at the GamePad's own size (854x480, times the Internal Resolution, or a GamePad resolution pack's size) instead of stretched to the TV's 1280x720, so RetroArch's scaling and shaders get the GamePad's pixels. The other layouts stay at the TV's size (NNshi).
- Controller > Rumble Strength goes in steps of 10% (Off, 10% to 100%) instead of 25% (NNshi).
- Vulkan: a title's pipeline cache (`shaderCache/transferable/<title>_vkpipeline.bin`) is closed when the title is closed, instead of when the next one starts, so it is no longer left locked on Windows (NNshi).
- New option Core > Audio Thread (Restart), off by default: RetroArch takes the audio on its own thread and the Wii U's audio runs by the clock, as in standalone Cemu, so it no longer breaks up while a frame is late, during loading or shader compilation (NNshi). With it on, Vertical Sync rather than Audio Sync paces the frames, and RetroArch's audio statistics stay empty.
- Vulkan: no crash when a game's shader finished compiling in the middle of a run of draws. The first draw had skipped while it compiled (Async Shader Compile), and the next one found no descriptor sets and stopped the core on an assert; it now makes them (NNshi, The Wonderful 101).
- Core options rearranged (NNshi): categories System, Graphics, Controller, Title Manager, Core, Logging, then Graphic Packs and Cheats. Video and Shaders are now Graphics, Screen is Controller (with Rumble Strength and GamePad Motion), Content and Convert to WUA are Title Manager, the accounts are under System, and the core's own options (Internal Resolution, Render Ahead One Frame, Reduce BC1 Texture Memory) are under Core. Every option that applies only once the game is started again says (Restart) in its name; Graphic Packs and Cheats say it in the category's.
- CPU Mode, Accurate Shader Multiplication and Precompiled Shaders follow the game profile on Auto, as standalone: Auto used to override the profiles, which set the CPU mode for 27 games and turn accurate multiplication off for Super Mario 3D World, Captain Toad and Xenoblade Chronicles X. Accurate Shader Multiplication now has Auto, the default.
- New option Graphics > Accurate Barriers (Vulkan), standalone's, on by default.
- The Skylanders, Disney Infinity and LEGO Dimensions add-on options are shown only while one of their games is loaded, below all categories.
- Graphics is Graphics (Restart): its options apply once the game is started again, except GX2DrawDone Sync and Accurate Barriers, whose help says they can be toggled during gameplay. An option for one graphics API says which in its name: Precompiled Shaders (OpenGL), Async Shader Compile (Vulkan), Accurate Barriers (Vulkan) (NNshi).
- Vulkan is the default graphics API, as in standalone Cemu (NNshi).
- Upscale Filter and Downscale Filter removed: they acted only on the GamePad picture in the composite screen layouts, never on the TV picture RetroArch scales (NNshi).
- Frame Wait removed: each run waits the whole frame time for the game's frame, as the default did. Waiting 72 % of it made no measurable difference on a PC or a phone (NNshi, sco8487).
- Internal Resolution is no longer hidden while a resolution graphic pack is on (NNshi).
- Show Game FPS is under Core, at the bottom: it shows the game's own frame rate where RetroArch's counter does not (NNshi).
- Options the core hides (the screen layout slots above Number of Screen Layouts) are hidden again right after the options are declared anew, which happens at a game's start; they showed in the menu until another option was changed (NNshi).
- Start Conversion to WUA is also offered for a `.wua` that lacks an installed update or DLC, or has an older version of one: a new `.wua` is written with them. Its help text says which of the three cases it is: the game alone, the game with its update and DLC, or a `.wua` to add them to. An existing file is never replaced; a taken name gets a number after it, where the conversion used to overwrite a `.wua` of the same name (sco8487).
- Sound: each frame hands RetroArch one frame's worth of audio, as other cores do, and more only to work off a backlog. It handed over whatever the game had made by the end of the frame, a different amount every frame, so RetroArch's audio buffer swung between blocking and running dry without settling (NNshi).
- Sound: the game's audio frames are made at the console's pace, one every 3 ms, and faster only to catch up. At 30 fps each frame's 33 ms of audio was made in a 19 ms burst followed by a 14 ms gap, so streaming voices were fed unevenly, with pops and crackles standalone Cemu does not have (NNshi, Wind Waker HD).

## October 2026

### Upstream

- Merged up to upstream Cemu 2.6-356 (aa20e2f8), after the merge of 3 October (32e6628a, Latte shader and texture fixes). The core reports upstream's version.

### Sound and pads

- GamePad Motion option (System, off by default as in standalone): the GamePad's gyroscope and accelerometer come from the sensors RetroArch reads on port 1, for games that aim or steer by tilting the GamePad. On Linux with the udev controller driver, a DualSense or DualShock 4 needs its motion sensors device made readable; the README has the rule (tested by Shoegzer, Splatoon with a DualSense).
- Audio Channels (System): Mono, Stereo or Surround 5.1 - what the console reports for the TV and AX mixes for. 5.1 goes to RetroArch's multi-channel output.
- Rumble through RetroArch's rumble interface, and a Pro Controller on port 1.

### Picture

- Internal Resolution as multiples of the game's own resolution (0.25x to 3x), with 16:9 render targets kept 16:9. The largest picture RetroArch is told to expect grows with it.
- The frame rate is always the graphic packs'; there is no Frame Rate option.
- A 30 fps game is reported to RetroArch as 30 fps: the core follows the game's own swap interval (GX2SetSwapInterval) once it has held for a second, where it reported 60 and showed every frame twice, unevenly (NNshi, Wind Waker HD).
- New option Video > Frame Wait: how long each run waits for the game's frame. Whole Frame, the default, gives the GPU the whole frame time as standalone Cemu does; Most of a Frame waits 72 % of it, as the core did before.
- New option Video > Render Ahead One Frame (off by default): the GPU draws the next frame while RetroArch presents the last, for one frame more latency, as standalone Cemu's swapchain has.
- Vulkan: a frame is handed to RetroArch only once its copy has been submitted; before, RetroArch could show the previous frame again (a repeated and a dropped frame, NNshi, Mario Kart 8).
- The Wii U's software keyboard (Wind Waker HD's name, for one) is typed into with RetroArch's keyboard (Game Focus on), the text shown in RetroArch's notifications; Start on the pad accepts, with the account's name when nothing was typed. Before, the game waited on a keyboard nobody could see.
- The pipeline cache keeps every pipeline compiled before the game was closed; the last ones were lost and compiled again at every start (NNshi, the same stutters in BotW and Mario Kart 8 each session).
- OpenGL shows both screens in the composite layouts.
- Android: Taiko's Don is drawn on Qualcomm's driver.

### Games and content

- New option Accounts > Rename Active Account: the account's name is entered inside the core, with a pad (Up and Down change the last character, Right adds one, Left removes one, Start accepts, Select cancels) or a keyboard, and shown as the frontend's status message while it is typed. `account_name.txt` is written with the new name. It needs the frontend's on-screen notifications on (Shoegzer, #28).
- Accounts can be named: `system/Cemu/account_name.txt` has a line per account, `<id> = <name>`, up to ten characters; the core writes the file with the accounts there are (at each title start and when an account is made or removed), each takes its name from it when a title starts, and the account lists in the options show it (Shoegzer #28, NNshi).
- Manage Content: Install Game and Uninstall Game, for the running game, into mlc01. Updates and DLC are installed from `system/Cemu/titles`, and installed content can be removed.
- Disc images have their keys beside them (`<image>.key`).
- Graphic packs: an Enabled switch per pack, with preset groups shown as standalone does.

### Stability

- Linux: the core no longer links libGL and libX11, so it loads on a system without X11. The GLX functions it shares RetroArch's OpenGL context with on an X11 session are found at run time, in RetroArch's own libraries.
- RetroArch can unload the core: the audio API and the graphic pack loading run on joined threads, the GPU thread ends properly on Windows, and unloaded modules are forgotten by the graphic packs, padscore and the DLC list.

### Options and logging

- Log Thread CPU Time: how busy each emulated core's thread, the GPU thread and the frontend's thread were, once a second; on Linux and Android also each core's clock against its maximum.
- A Dump Shaders option. Logging is the last category.
- Options that need a restart say so at the end of their description.

## September 2026

### Upstream and the repository

- Merged upstream Cemu of 12 September, a month of texture correctness fixes.
- The repository builds the core only: the wx GUI application, the standalone's input, audio and other backends and its GitHub workflows are removed.
- The core option texts are translated through libretro's Crowdin project.

### Graphic packs

- The community graphic packs ship inside the core, and the loaded game's packs are offered as core options, only those for the running title. A resolution pack decides the render scale and the size the picture is presented at.
- `settings.xml` is not read.

### Picture

- The frontend decides when a frame happens: the emulated cores and the GPU thread stop with the frame, and retro_run stays at 60 Hz whatever the title renders at.
- Screen layouts for the TV and the GamePad, with a button or key that steps through them, and Picture in Picture.
- Internal Resolution changes the internal resolution, down to below 720p.
- BC textures are decoded where the device has no BC formats, in integers, optionally into 16 bits.
- The shader cache's loading progress is shown by RetroArch.

### Sound

- Audio is paced by what RetroArch asks for, and the game's frame rate can be shown.

### Pads

- Ports 2 to 4 can be a Wii Remote, a Wii Remote held sideways, a Pro Controller or a Classic Controller. Stick clicks reach the GamePad.

### Games and content

- A title can be converted to `.wua` from a menu, written through RetroArch's file system, while the title keeps running.
- A place to put updates and DLC; the default mlc01 folders and files are created as standalone Cemu does.
- Paths are read as UTF-8 on Windows.

### Stability

- Closing content stops the title's threads, the GPU thread, the IOSU services, the shader and pipeline compilers and the cache writers, and a second title can be loaded after the first.
- Reset is a reset.
- The core stays mapped when RetroArch unloads it.

### Builds

- Cores for iOS, tvOS and webOS.

## August 2026

- The core on libretro's buildbot: Linux x64, Windows (MinGW) and Android.
- Content and Wii U archives are read through RetroArch's VFS.
- Vulkan: the frontend's context is kept across a video driver rebuild, the frontend's queue lock is taken when submitting, and the first draw no longer crashes on the frontend's device. Vulkan is the default where the core has no OpenGL backend.
- Android: the core builds and starts, with fibers without ucontext, EGL and an Android Vulkan surface.
- Windows: the core builds, with a WGL path.
- Wii Remotes are driven from libretro pads.
- Closing content returns from retro_unload_game instead of ending the process, and the title is shut down first so saves reach disk; mlc01 is kept where RetroArch puts saves.
- A missing disc key is reported, and the title stops.
- A multi-core interpreter CPU mode.
- Logging options for system APIs, thread synchronisation, file access and a Wii U thread snapshot.

## March to July 2026

- Linux support for the core, and Vulkan through RetroArch's hardware render interface.
- The GamePad's display modes on Linux, composed into the Vulkan picture.
- A Linux aarch64 build; `.tmd` accepted as content; the core info file.

## December 2025

- libretro core support, and reading files through libretro's VFS, by danprice142: the original port this fork builds on.
