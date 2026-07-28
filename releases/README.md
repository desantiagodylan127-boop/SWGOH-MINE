# Heroes Offline test builds

Latest playtest shell + source ShadowHook bridge:

`HeroesOffline-playtest-shadowlogin4.zip`

- Fixes r3 crash: login original ABI was inverted (bool before auth string).
- Uses offline12 guest-login sibling path (AuthSelected / PostAuth / Finish).
- Hooks only HTTP + env-list + login (AssetBundle / ImportAccount still off).

https://media.githubusercontent.com/media/desantiagodylan127-boop/SWGOH-MINE/cursor/dobby-hook-shim-528d/releases/HeroesOffline-playtest-shadowlogin4.zip
