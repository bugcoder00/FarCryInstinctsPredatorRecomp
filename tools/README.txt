FAR CRY INSTINCTS PREDATOR RECOMP 1.0.0
=======================================

PC version of Far Cry Instincts Predator (Xbox 360), made by static recompilation.
Not affiliated with Ubisoft or Microsoft. NO game files are included (levels, graphics, sounds,
videos): you need your own copy of the game.

WHAT YOU NEED
-------------
- 64-bit Windows 10 or 11.
- A processor with 4 cores and 8 threads.
- 8 GB of RAM (the game uses ~1.5 GB).
- A DirectX 12 graphics card (integrated graphics works).
  The launcher shows what your PC has and whether it meets this minimum.
- YOUR copy of Far Cry Instincts Predator for Xbox 360 (USA/Europe version): the disc .iso file
  (the game runs straight from it, no extracting) or the files extracted to a folder (the folder must
  have default.xex and the game's .dat files).

HOW TO PLAY
-----------
1. Extract this package to a folder you can write to (for example Documents\FarCryPredatorRecomp).
   Avoid "Program Files": the game saves games and options in its own folder.
2. Open PredatorGame.exe. The launcher appears. The language (Portuguese or English) is changed at the top
   right, in "IDIOMA - LANGUAGE"; the first time it follows the Windows language. In Portuguese the game's
   menus and texts are also in Portuguese (applied on the fly, your copy's files are not changed); in English
   the game keeps the disc's original texts.
3. In "Game folder", click "ISO file..." and choose the disc .iso, or "Folder..." and choose the
   folder with default.xex.
4. Adjust video and controls if you want and click Play.
   The first time, the program prepares the files and the graphics: it may take about 1 minute longer.
   "Internal resolution" 1.5x or 2x makes the image sharper but is much heavier on the graphics card
   (on integrated graphics use 1x). 3x (4K) is experimental: it needs a strong graphics card.
   "Frames per second": 30 (like the original game), 60 (smoother, twice the load on the graphics card) or
   unlocked (experimental).
   "Aspect ratio": "Monitor" is for ultrawide monitors (21:9, 32:9; experimental): the game shows more of
   the scene on the sides and the HUD and menus stay in the center. Use it in full screen; 2x looks sharper.
   "16:9 with black bars" shows the game like the original, without stretching, with black bars on the sides.
   The Save button stores the options without opening the game.
   "Shadows" high or very high makes shadows much sharper; high costs little.

MAP EDITOR: choose MAP EDITOR in the game's main menu. The editor is PredatorEditor.exe (it sits next
to PredatorGame.exe and is opened by the menu by itself); "EXIT EDITOR" goes back to the game. It uses
ige.xex and farcryed.dat from your copy of the game.

Checked "Start the game directly"? To see the launcher again, open PredatorGame.exe holding Shift.

HELP REDUCE STUTTERS: open the game through collect_recipe.bat instead of PredatorGame.exe. It writes to the
"recipes" folder which graphics effects (shaders) showed up while you played (numbers only, no game data). Send that
folder to the developer and the next version will have those effects prepared, with no stutter the first time.

CONTROLS
--------
- Controllers: DualSense and DualShock 4 (USB or Bluetooth, no extra programs) and Xbox controllers.
- Keyboard and mouse (default): W A S D move, mouse look, left button shoot, right button aim,
  Space = A (jump), T = B, R = X, F = Y, Q = LB, E = RB, Enter = Start, Esc = Back.
- In menus the mouse becomes a pointer: hover an option and it is selected right away, left click
  confirms, right click goes back and the wheel moves up/down. It also works on screens with values
  (Controller options), on the multiplayer character customization and in the map editor. When the
  screen changes the cursor stays where it is. With the F1 window open the mouse is free. Press the
  controller and the arrow hides; move or click the mouse and it comes back.
- With player 1 on the keyboard, the button icons in the game texts become the configured keys
  (e.g. Space instead of A; the mouse instead of the triggers), and the "Controller setup" screen shows
  a keyboard and a mouse with "Press F1 to see or change the controls". A controller connected for
  player 2 does not change this; if player 1 switches to a controller, the buttons and the controller
  picture come back. With "Merge" on (keyboard and mouse on player 1 and a controller on player 2), the
  icons follow what you used last: press the controller and its buttons show; use the keyboard or
  mouse and the keys show.
- In the map editor the mouse turns the camera, and moving the mouse forward and back zooms in and
  out. In the object tool, hold T (the "ROTATE") and move the mouse sideways to rotate the object. In
  "Jump in map" the mouse aims like in the game again. The pointer also works in the editor's pause
  menu (Enter inside the map). In that menu's "Controls" (controller map), with the keyboard a
  keyboard picture shows and each text ends with a picture of its key (e.g. "TEST MAP:" with the TAB
  key drawn).
- Players 1 to 4 choose their devices with the "Controls" button (split screen for up to 4 people):
  keyboard and mouse, "Controller" (the first one connected) or a connected controller (DualSense,
  Xbox...). The same device cannot be used by two players.

- The profile menu (Options) has "VIDEO OPTIONS": internal resolution, shadows, anti-aliasing, frames
  per second and vertical sync, the same options as the launcher (changes there are saved for the
  launcher too). Everything changes right away (with internal resolution and shadows the image may
  flicker once). Window size and aspect ratio stay in the launcher.
- The main menu has "QUIT GAME", which closes the game (the same as closing the window).

PROFILES
--------
In the launcher, "Profile" chooses who is playing. Each profile has its own saved games and progress.
To create a profile, type a new name (up to 15 letters, no accents) and click Play. The main profile
("User 1") uses the "save" folder; the others are in "save\perfis\<name>".

KEYS DURING THE GAME
--------------------
F1          controller settings (to change a key: click the field and press the key, the mouse
            button or turn the wheel; right click on the field leaves it empty; if the key was already
            used by another action it moves: that field flashes red and is left empty)
F2          video options (they change right away and are saved in opcoes.ini)
F3          release/capture the mouse in the window
+ and -     mouse sensitivity
Alt+Enter   full screen / window
F12         save a problem report ("bugs" folder) when something goes wrong
            (if the graphics card stops, the game creates this folder by itself and shows where it is)

CHEAT CODES: in the main menu, CHEATS MENU opens a small box to type the code with the
keyboard (Enter confirms, Esc or B on the controller cancels). Or turn the cheats on in the
launcher CHEATS tab (each one has a description and works without typing; there is also "Invincible").

LOCAL NETWORK PLAY (SYSTEM LINK)
--------------------------------
Two or more PCs on the same network (cable or Wi-Fi, same router), each with the game installed.
1. On one PC: MULTIPLAYER > SYSTEM LINK > X (CREATE MATCH) > CREATE MATCH.
2. On the others: MULTIPLAYER > SYSTEM LINK, choose the match in the list and press A.
3. Everyone presses X (READY) and whoever created it chooses LAUNCH.
IMPORTANT: the first time, Windows asks whether PredatorGame may use the network. Click "Allow".
If the match does not show in the list, check in Windows (Settings > Network & Internet > Wi-Fi or
Ethernet) that the network is a "Private network" and that PredatorGame is allowed in Windows Firewall.
The player name in the match is your Windows user name.

INTERNET PLAY (SYSTEM LINK, "JOIN BY IP")
-----------------------------------------
One PC creates the match (the HOST) and friends join over the internet, without a VPN program.
Everyone talks only to the host, so only the host needs open ports on the router.

WHOEVER CREATES THE MATCH (HOST)
1. Open PredatorGame.exe. In the launcher, leave "Host IP" EMPTY and click MY IP.
   Your internet IP shows up (it is already copied: paste it with Ctrl+V in the chat with friends).
2. Click Play. In the game: MULTIPLAYER > SYSTEM LINK > X (CREATE MATCH) > CREATE MATCH.
3. When creating the match the game tries to open the router ports by itself (UPnP) and closes
   them when you quit. The result is in fcrecomp_log.txt (lines starting with [upnp]).
4. The first time, Windows asks whether PredatorGame may use the network: click "Allow"
   (if shown, check private AND public networks).
5. When everyone is in the lobby and READY, choose LAUNCH.

IF THE ROUTER DOES NOT OPEN THE PORTS BY ITSELF
- Many routers come with UPnP off. Either turn UPnP on in the router page, or open the ports by
  hand: in the router page (usually http://192.168.0.1 or http://192.168.1.1), look for
  "Port Forwarding", "Virtual Server" or "NAT" and create rules for YOUR PC's IP (it shows in
  fcrecomp_log.txt as "[lan] o jogo usa o endereco ..."):
      UDP 1000, UDP 1004, UDP 1005, UDP 1010 and UDP 49001  (external port = internal port)
  Tip: make your PC's IP fixed in the router ("DHCP reservation"), otherwise the rule stops working.
- To check UPnP: open cmd in this folder and run  PredatorGame.exe --upnp-teste
  (the result goes to upnp_teste.txt).
- CGNAT: some providers (very common on fiber, 4G/5G and radio internet) do not give your router
  a real internet IP. The MY IP button warns when this happens. With CGNAT opening ports does NOT
  help: ask the provider for a "public IP" (or static IP), or use a virtual network such as Radmin
  VPN or ZeroTier (then just play as on a local network, without typing any IP).

WHOEVER JOINS (FRIENDS)
1. Open PredatorGame.exe. In the launcher, in "Host IP", type the IP the host sent
   (example: 177.45.10.20). Click Play. (The IP stays saved for next time; to play only on the
   local network again, clear the field.)
2. In the game: MULTIPLAYER > SYSTEM LINK. The host's match shows in the list: choose it and press A.
3. The first time, click "Allow" in the Windows Firewall warning.
4. Press X (READY) and wait for the host to choose LAUNCH.
Whoever joins does NOT need to open ports on the router.

IF IT DOES NOT SHOW IN THE LIST
- The host must have the match already created (in the lobby) before you search.
- Check the IP (it must be the internet one, from the MY IP button, not 192.168...).
- Check that the host opened the ports (or that UPnP worked) and clicked "Allow" in the Firewall.
- If the host has CGNAT, only with a virtual network (see above).
- The game was made for local networks: with slow or distant internet there may be delay ("lag").

FILES THE GAME CREATES IN THIS FOLDER
-------------------------------------
image.bin          prepared from your default.xex the first time (do not share it: it is game data)
image_editor.bin   the same for the map editor (from ige.xex), the first time it opens
save\              saved games (main profile; other profiles in save\perfis)
opcoes.ini         launcher options
controles.ini      controls
fcrecomp_log.txt   log (send it along when reporting a problem)

                                   Author: bugcoder00
                                     Version 1.0.0
