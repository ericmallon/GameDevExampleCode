# GameDevExampleCode

Some snippets of code from Midair: Community Edition (https://midair.gg/) that I wrote. Onboarding players was our #1 difficulty, so I focused on bots, practice mode, and tutorials. I also implemented a variety of other requests from the community and assorted features I thought would be valuable.

Some of my work on the game includes:
* Creating Bots/their AI from scratch
* Pub Starter - Allows server admins to configure their server to enable 'pub starter', which will spawn bots to ensure a targeted player count, removing them as real players join.
* Practice Mode - Recording of player movement, playback of those recorded movement trails, drills with a variety of victory conditions, saving/loading practice data from files.
* Tutorial Builder - built on top of practice mode, allows players to create their own tutorials to share. 
* Public game AFK timer, auto force spectate and then kick depending upon configurable server settings based on # players and max server capacity.
* Various demo playback enhancements to improve montage creation (extra zoom, fine speed controls, etc)
* Tracking of many new player match statistics
* Added custom reticles, Player IFF scaling, crosshair scaling
* Blueprint based UI work

MABotAiComponentExample.cpp - Primary AI driver for our bots. This is the whole file.
MAPracticeComponentExamples.cpp - A few snippets from the code driving my practice mode
MAWeaponComponentExample.cpp - One function showing an implementation of a weapon overheat system.
