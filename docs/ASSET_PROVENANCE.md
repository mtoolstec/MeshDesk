# Assets and provenance

Current app packages use LVGL built-in symbol glyphs and firmware-drawn geometric icons (including the cloud icon). Their Font Awesome/Montserrat notices are in licenses/fonts. The Chinese font records its SourceHanSansSC generation input and preserves the corresponding font license.

DeepSeek Balance bundles an original geometric cloud in `apps/deepseek/icon.svg`
and its 32×32 transparent PNG export, `icon.png`, under the project GPL-3.0-only license.
The PNG is included in the app package so restoring the app also restores its
desktop icon; it does not use a third-party brand logo.

The monochrome emoji fallback is generated from Google Fonts' Noto Emoji
(`ofl/notoemoji/NotoEmoji[wght].ttf`) at revision
`8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5`, SHA-256
`de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551`.
Its SIL OFL 1.1 license is preserved in `licenses/fonts/NotoEmoji-OFL.txt`.
`scripts/generate_emoji_font.py` exports weight 400, 14px, 2bpp with
lv_font_conv 1.5.3. The 1,471 non-ASCII glyphs include U+1F44D (thumbs up).
The bitmap renderer uses monochrome component glyphs; it does not compose
multi-codepoint flags, skin-tone variants, or ZWJ emoji sequences.

The following historical assets had no verifiable source or license record. They were removed from the current tree and PNG manifest references replaced with built-in glyphs. The redundant 2048 ZIP was removed so the source application is the only distributable package. This is not a claim that legacy images were licensed under MIT.

| Removed path | SHA-256 |
|---|---|
| `apps/adsb/icon.png` | `45904092056a26eac846daceff3e4032a5b0d2f455925ef37570444d96b342a9` |
| `apps/dinosaur/icon.png` | `2a28c0818df7c7acbee693431082ae7491c9b35552bc96d6a1dc955f79ff1aeb` |
| `apps/matrix/icon.png` | `49110896d20d2e4adaca98abfe4871b7634a6a6114ec246cc6aa39060c74db58` |
| `apps/speed/speed.png` | `02f94d4b62c644d9315aea624600bd29ee25898444c919bbc10a6d1474fc448d` |
| `apps/tetris/icon.png` | `2e392184836e774db89a0536553cd172453e3aae80fc4cacb7fb666c34ed259e` |
| `apps/speed/speed.bmp` | `9a2f57bd26a56810272e7d67e39c8da63a6e98603f7c8882156f65976137fc1e` |
| `apps/2048.zip` | `f8ce748fc588826d0367f1a427ac6bf8fa1cd90819847009f06aac246e423909` |

Earlier private revisions contained these resources. The current public baseline starts with a new root commit and excludes that history. The archived private history must not be imported into a public repository without resolving those assets.

Downloaded maps, screenshots and SD appdata are runtime data, not bundled assets or GPL-covered map content.
