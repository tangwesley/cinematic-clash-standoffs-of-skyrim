Cinematic Clash - HUD skin
==========================

Drop PNG files in this folder to replace the drawn parts of the clash meter.
Every file is optional: a part with no PNG keeps the built-in vector drawing,
so you can skin one element at a time. Transparency is honoured (straight,
non-premultiplied alpha, as exported by any normal image editor).

All files are drawn on ONE shared pixel grid. The grid width is set by
iTextureGridWidth in CinematicClash.ini (default 1024): a PNG that many pixels
wide is shown as wide as the bar, which is 32% of the screen width (times
[HUD] fScale). Everything else is scaled by that same ratio. So at 1080p one
texture pixel is 0.6 screen pixels, at 1440p 0.8, and at 4K 1.2.

Layout (top to bottom, all centred horizontally on the screen):

    [attack button glyph]                     above the frame, only during
                                              the mash, pulsing
    [frame.png]                               centred on fVerticalPosition
        [fill_player.png] [fill_opponent.png] centred inside the frame
        [marker.png]                          slides along the fill
    <player name>              <opponent name> text, under the frame
    [timer.png]                               under the labels

Files and recommended sizes (on the default 1024 grid; these match the
proportions of the built-in drawing, but any size works):

  frame.png          1040 x 56    Backing behind the bar: border, decoration,
                                  the centre tick if you want one. Nothing is
                                  drawn on top of it except the fills, marker
                                  and text.

  fill_player.png    1024 x 40    The player's share (built-in: gold). Its WIDTH
                                  IS THE METER: the left edge is 0%, the right
                                  edge is 100%. It is revealed from the left up
                                  to the current balance, never stretched, so
                                  paint the full bar and let the crop do the
                                  work. Should be the same size as
                                  fill_opponent.png.

  fill_opponent.png  1024 x 40    The opponent's share (built-in: red),
                                  revealed from the right.

  marker.png           12 x 60    The moving marker at the balance point,
                                  centred on the split. Make it taller than
                                  the fills so it overhangs top and bottom.

  timer.png          1024 x 8     Time remaining. Shown full width at the start
                                  of the mash and shrinks towards the centre
                                  (cropped, not stretched).

Attack button glyph
-------------------
During the mash the button the game has bound to attack is shown above the
bar, pulsing. It follows the device the game currently treats as active: an
Xbox-style pad button when a controller is in use, otherwise the mouse button
or keyboard key. The built-in drawing covers pad face buttons (A B X Y),
bumpers, triggers, stick clicks, the d-pad, Start/Back, any mouse button and
any keyboard key (named through your keyboard layout).

To use your own art, add key_<LABEL>.png, where LABEL is the name the glyph
would show, upper case, spaces as underscores. Recommended size about 66 x 66
on the default grid (that is 40 px tall at 1080p, the same as the drawn
glyph; wider is fine for triggers or long key names). Examples:

  key_RT.png  key_LT.png  key_A.png  key_B.png  key_X.png  key_Y.png
  key_LB.png  key_RB.png  key_LS.png  key_RS.png  key_DPAD_UP.png
  key_START.png  key_BACK.png
  key_LMB.png  key_RMB.png  key_MMB.png  key_M4.png  key_WHEEL_UP.png
  key_E.png  key_SPACE.png  key_LEFT_SHIFT.png  key_F.png

Set [Debug] bDebugLog = 1 and start a clash: the SKSE log prints the label
the glyph resolved to, so you know which file name it is looking for.

Not skinnable: the two names under the bar are text, since they change per
character and per fight. They are drawn in the same font as the rest of the
game's HUD (whatever Interface\fontconfig.txt maps $EverywhereFont to, so font
replacer mods carry over). Point sFontFile in CinematicClash.ini at a .ttf
under Data to use a different font instead; sGameFont picks another mapping.
The same font is used for the lettering on drawn glyphs.

Iterating: the skin and font are reloaded from disk every time
CinematicClash.ini is saved, so you can tweak PNGs with the game running and
save the INI (even unchanged) to see the result on the next clash.
