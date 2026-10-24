import colorsys, sys
# Ableton Dark, re-derived from the user's Live 12 Arrangement View screenshot (T3.2-fix1, Q47; the screenshot is
# git-ignored at reference/inspiration/live12/arrangement.png). "S:" = sampled there (pixel value, where), "own:"
# = not visible in it, chosen to sit in the sampled palette. Live renders text with ClearType, so text colours
# are the glyph-core value of large / bold glyphs.
colors = [
 ("surface.background", "#16282E"),   # S: menu bar, transport bar, gaps between panels
 ("surface.panel", "#26383F"),        # S: browser, detail view, status bar, track-header area, transport fields
 ("surface.raised", "#3D4F55"),       # S: browser column-header strip, Tuning / Groove panel titles
 ("surface.sunken", "#0F2026"),       # S: header choosers (Mixer, Track Volume), routing fields
 ("surface.divider", "#0F2026"),      # S: 2-px dividers between track headers and browser columns
 ("surface.highlight", "#3D4F55"),    # S: selected track row in the lanes
 ("control.fill", "#08191F"),         # S: S / H / W / Set / Pause buttons
 ("control.fillHover", "#14272E"),    # own: one step toward the panel
 ("control.fillPressed", "#031117"),  # S: meter well (the darkest well in Live's panels)
 ("control.fillDisabled", "#1B2D33"), # own: between control.fill and the panel
 ("control.border", "#000A0F"),       # S: 1-px edge of the header buttons and number boxes
 ("control.track", "#0F2026"),        # S: unfilled part of the Main volume box
 ("control.value", "#007969"),        # S: value fill of Main volume, Global Amount 100 %, 1.00x
 ("control.valueDisabled", "#4A5D63"),# own
 ("control.thumb", "#B0B5A3"),        # own: the ruler-text colour
 ("control.thumbHover", "#D6CDA8"),   # own: text.primary
 ("control.needle", "#D6CDA8"),       # own: text.primary
 ("field.fill", "#0F2026"),           # S: header choosers (Mixer, Track Volume), routing 1/2
 ("field.fillHover", "#16282E"),      # own: surface.background
 ("field.border", "#000A0F"),         # S: chooser / number-box edge
 ("field.borderFocus", "#FFC658"),    # own: accent.focus
 ("text.primary", "#D6CDA8"),         # S: browser items, "Drop Files and Devices Here", button labels
 ("text.secondary", "#BCC0AE"),       # S+: bar-number / time-ruler text #B0B5A3 lightened to 4.5:1 on surface.raised (#B0B5A3 is 4.1:1 there;
                                      #     Live's dimmer label grey #82918F is 3.7:1 on the panel, so it became text.disabled)
 ("text.disabled", "#82918F"),        # S: "Edit", panel titles ("Collections", "Library"), overview frame
 ("text.onAccent", "#0F2026"),        # S: number in a lit track activator
 ("text.selection", "#2F566E"),       # own: steel blue, text.primary >= 4.5:1 on it
 ("text.caret", "#F2EDD8"),           # own
 ("accent.primary", "#FFC658"),       # S: lit loop switch, track activators, status-bar button
 ("accent.activator", "#FFC658"),     # S: lit track activator (= mixer.activatorOn)
 ("accent.solo", "#5EA8E6"),          # own (no lit solo in the screenshot; = mixer.soloOn)
 ("accent.arm", "#FF5559"),           # S: lit arm button (= mixer.armOn)
 ("accent.focus", "#FFC658"),         # own: accent.primary
 ("selection.fill", "#3D6787"),       # own: steel blue (menus are not in the screenshot)
 ("selection.text", "#FFFFFF"),
 ("menu.fill", "#16282E"),            # own: surface.background (menus are not in the screenshot)
 ("menu.border", "#000A0F"),          # own: control.border
 ("menu.separator", "#3D4F55"),       # own: surface.raised
 ("menu.shadow", "#00000059"),
 ("tooltip.fill", "#3D4F55"),         # own: surface.raised
 ("tooltip.border", "#000A0F"),
 ("tooltip.text", "#EEE8D0"),         # own: a lighter text.primary
 ("track.header", "#26383F"),         # S: track-header area (= arrangement.header)
 ("track.headerSelected", "#3D4F55"), # S: selected track's header row
 ("track.lane", "#26383F"),           # S: arrangement lane (= arrangement.lane)
 ("track.laneAlt", "#223339"),        # S: alternate beat shading (= arrangement.laneShade)
 ("track.laneSelected", "#3D4F55"),   # S: selected track lane (= arrangement.laneSelected)
 ("clip.text", "#000A0F"),            # S: darkest sampled ink (control.border); #0F2026 would give 4.4:1 on the worst palette colour
 ("clip.textLight", "#F2F2F2"),
 ("clip.border", "#000A0F"),
 ("clip.selectedBorder", "#F2F2F2"),
 ("grid.bar", "#00000070"),
 ("grid.beat", "#00000040"),
 ("grid.sub", "#00000024"),
 ("grid.shade", "#00000014"),
 ("scrollbar.track", "#0F2026"),      # own: field well
 ("scrollbar.thumb", "#52666B"),      # own
 ("scrollbar.thumbHover", "#6B7E83"), # own
 # Arrangement view (T3.8): sampled from the user's Live 12 arrangement screenshot.
 ("arrangement.lane", "#26383F"),
 ("arrangement.laneShade", "#223339"),
 ("arrangement.laneSelected", "#3D4F55"),
 ("arrangement.gridLine", "#0D1317"),
 ("arrangement.separator", "#0F2026"),
 ("arrangement.background", "#16282E"),
 ("arrangement.header", "#26383F"),
 ("arrangement.ruler", "#26383F"),
 ("arrangement.rulerText", "#B0B5A3"),
 ("arrangement.loopFill", "#607175"),
 ("arrangement.loopEdge", "#B0B5A3"),
 ("arrangement.loopOffFill", "#37474D"),
 ("arrangement.loopOffEdge", "#607175"),
 ("arrangement.playhead", "#B0B5A3"),
 ("arrangement.startMarker", "#00CCB7"),
 ("arrangement.overviewFrame", "#82918F"),
 # Roll tokens follow the blue-grey palette of the Live 12 reference screenshot (Q47), like the base tokens above.
 ("roll.keyWhite", "#BCC6CA"),
 ("roll.keyBlack", "#16282E"),
 ("roll.keyText", "#141B1F"),
 ("roll.rowNatural", "#26383F"),
 ("roll.rowAccidental", "#1F3036"),
 ("roll.rowScale", "#33474E"),
 ("roll.rowLine", "#0000003A"),
 ("roll.equave", "#0D1317C8"),
 ("roll.outside", "#0000004D"),
 ("roll.offGrid", "#62D4E8"),
 ("roll.fundamental", "#62D4E88C"),
 ("roll.harmonic", "#62D4E826"),
 # Scale / MOS layouts (T3.12): gutter keys by step class (tonic, large step, small step at full hardness, extra
 # MODMOS / Scala steps pull toward keyExtra), the pitch line of a proportional row, the background EDO overlay.
 ("roll.keyTonic", "#F2C77E"),
 ("roll.keyLarge", "#BCC6CA"),
 ("roll.keySmall", "#4A6670"),
 ("roll.keyExtra", "#B98AE0"),
 ("roll.pitchLine", "#0000004A"),
 ("roll.edoLine", "#62D4E844"),
 ("roll.edoEquave", "#62D4E899"),
 # MOS wheel region chart (T3.13): surfaces from the Live 12 screenshot (arrangement.*), data colours from
 # mos-explorer (EDO blue, hardness amber, generator marker red).
 ("mosChart.background", "#16282E"),
 ("mosChart.grid", "#1E3238"),
 ("mosChart.axis", "#26383F"),
 ("mosChart.axisBorder", "#3D4F55"),
 ("mosChart.axisText", "#B0B5A3"),
 ("mosChart.axisTick", "#82918F"),
 ("mosChart.axisSubTick", "#52666B"),
 ("mosChart.periodLine", "#FFA33D66"),
 ("mosChart.edoLine", "#6EB5FF66"),
 ("mosChart.edoText", "#6EB5FF"),
 ("mosChart.edoCents", "#82918F"),
 ("mosChart.edoHardness", "#F5A623"),
 ("mosChart.marker", "#F0566E"),
 ("mosChart.markerText", "#16282E"),
 ("mosChart.sizeText", "#82918F"),
 ("mosChart.bandText", "#FFFFFF"),
 ("mosChart.bandSubText", "#D3D8CF"),
 ("mosChart.bandTextDim", "#5D6E73"),
 ("mosChart.bandGrayed", "#7F8F94"),
 ("mosChart.bandActive", "#FFFFFF"),
 # MOS wheel circle view (T3.14): surfaces as mosChart.*, data colours from mos-explorer drawMOSCircle (tonic red,
 # JI / sharp amber, flat / child / mode-name blue, MODMOS green, all-intervals violet).
 ("mosCircle.background", "#16282E"),
 ("mosCircle.ring", "#3D4F55"),
 ("mosCircle.spoke", "#8E9AA8"),
 ("mosCircle.dot", "#FFFFFF"),
 ("mosCircle.dotEdge", "#16282E"),
 ("mosCircle.degreeText", "#B0B5A3"),
 ("mosCircle.tonic", "#E94560"),
 ("mosCircle.altUp", "#F5A623"),
 ("mosCircle.altDown", "#6EB5FF"),
 ("mosCircle.ji", "#F5A623"),
 ("mosCircle.modmos", "#50C878"),
 ("mosCircle.info", "#F5A623"),
 ("mosCircle.modeName", "#6EB5FF"),
 ("mosCircle.parent", "#F5A623"),
 ("mosCircle.child", "#6EB5FF"),
 ("mosCircle.dimText", "#82918F"),
 ("mosCircle.sliderTrack", "#26383F"),
 ("mosCircle.sliderThumbEdge", "#FFFFFF"),
 ("mosCircle.button", "#26383F"),
 ("mosCircle.buttonBorder", "#3D4F55"),
 ("mosCircle.buttonText", "#B0B5A3"),
 ("mosCircle.periodMark", "#F5A62388"),
 ("mosCircle.octaveMark", "#6EB5FF88"),
 ("mosCircle.allIntervals", "#9B7EDE"),
 ("mosCircle.guide", "#FFFFFF"),
 # Track-header mixer section and mixer view (T3.10): sampled from the user's Live 12 screenshot (header boxes,
 # activator, arm, Pre/Post, the master's volume / cue boxes); soloOn and sendFill are not in it (own values).
 ("mixer.background", "#26383F"),
 ("mixer.separator", "#0F2026"),
 ("mixer.boxBorder", "#000A0F"),
 ("mixer.field", "#0F2026"),
 ("mixer.fieldText", "#D6CDA8"),
 ("mixer.buttonOff", "#08191F"),
 ("mixer.buttonText", "#D4CCA7"),
 ("mixer.activatorOn", "#FFC658"),
 ("mixer.activatorText", "#0F2026"),
 ("mixer.soloOn", "#5EA8E6"),
 ("mixer.armOn", "#FF5559"),
 ("mixer.postOn", "#FFC658"),
 ("mixer.volumeFill", "#007969"),
 ("mixer.panFill", "#3C6AB6"),
 ("mixer.sendFill", "#4D6E78"),
 ("mixer.meterWell", "#031117"),
 ("mixer.selected", "#3D4F55"),
 # Automation (T3.15): lanes a shade darker than arrangement.lane, Live's red envelope and breakpoints, grey when
 # the automation is overridden, the orange "Re-enable Automation" button.
 ("automation.lane", "#22343A"),
 ("automation.line", "#FF5A5F"),
 ("automation.lineOverridden", "#7D8C90"),
 ("automation.unautomated", "#FF5A5F8C"),
 ("automation.point", "#FF5A5F"),
 ("automation.pointSelected", "#FFFFFF"),
 ("automation.pointEdge", "#0F2026"),
 ("automation.laneHeader", "#1F3036"),
 ("automation.laneHeaderText", "#B0B5A3"),
 ("automation.veil", "#16282E59"),
 ("automation.selection", "#FFFFFF24"),
 ("automation.readout", "#0F2026E6"),
 ("automation.readoutText", "#F2F2F2"),
 ("automation.led", "#FF5A5F"),
 ("automation.ledOverridden", "#7D8C90"),
 ("automation.reenable", "#FFA33D"),
 ("automation.reenableText", "#0F2026"),
]
# Sizes (T3.2-fix1): Live 12 at 100 % draws 7-px capitals (browser items, "Drop Files and Devices Here"); Inter 13 px
# draws 9-10. Body text goes 13 -> 12 and titles 14 -> 13. Not smaller: 11 px is a size the scene fonts do not
# already hold, so it would add two font roles per UI scale to the glyph atlas (measured: the 5-scale atlas no
# longer fits one R8 page), and Inter below 12 px loses legibility at 1.0x. Controls are square in Live 12
# (transport fields, header buttons); panels keep a 3-px radius (measured on the browser / detail-view corners).
# Transport fields are 17 px high, track and browser rows 19 px.
fonts = [("small","regular",12,False),("body","regular",12,False),("bodyStrong","semibold",13,False),("title","semibold",13,False),("numeric","regular",12,False)]
metrics = [("spacing.xs",2),("spacing.sm",4),("spacing.md",8),("spacing.lg",12),
 ("radius.control",0),("radius.panel",3),("radius.menu",3),("radius.clip",2),
 ("size.control",17),("size.row",19),("size.menuItem",20),("size.scrollbar",10),("size.knob",34),("size.meter",14)]

# Clip palette: 13 hues + a grey column, 5 rows (vivid, light, mid, muted, dark). Own values: each row has a
# target relative luminance, so a clip name is readable on every colour with dark text (rows 0-3) or light
# text (row 4) at >= 4.5:1.
def lin(c): return c/12.92 if c<=0.04045 else ((c+0.055)/1.055)**2.4
def lum_rgb(r,g,b): return 0.2126*lin(r)+0.7152*lin(g)+0.0722*lin(b)
def hexc(r,g,b): return "#%02X%02X%02X" % (round(r*255), round(g*255), round(b*255))
def for_lum(h, s, target):
    lo, hi = 0.0, 1.0
    for _ in range(60):
        m = (lo+hi)/2
        r,g,b = colorsys.hls_to_rgb(h/360, m, s)
        if lum_rgb(r,g,b) < target: lo = m
        else: hi = m
    r,g,b = colorsys.hls_to_rgb(h/360, (lo+hi)/2, s)
    return hexc(r,g,b)
hues = [0, 14, 28, 40, 52, 70, 100, 140, 168, 188, 205, 228, 270]
rows = [(0.78, 0.34), (0.70, 0.62), (0.48, 0.23), (0.30, 0.44), (0.50, 0.075)]
greys = [0.55, 0.80, 0.25, 0.40, 0.06]
pal = []
for r,(s,t) in enumerate(rows):
    for h in hues: pal.append(for_lum(h, s, t))
    lo, hi = 0.0, 1.0
    for _ in range(60):
        m=(lo+hi)/2
        if lum_rgb(m,m,m) < greys[r]: lo=m
        else: hi=m
    pal.append(hexc(m,m,m))
o = []
o.append('{\n  "name": "Ableton Dark (sampled from Live 12)",\n  "colors": {\n')
o.append(",\n".join('    "%s": "%s"' % kv for kv in colors) + "\n")
o.append('  },\n  "fonts": {\n')
o.append(",\n".join('    "%s": {"face": "%s", "px": %d, "tabular": %s}' % (n,f,p,"true" if t else "false") for n,f,p,t in fonts) + "\n")
o.append('  },\n  "metrics": {\n')
o.append(",\n".join('    "%s": %s' % (n, v) for n,v in metrics) + "\n")
o.append('  },\n  "clipPalette": [\n')
lines = []
for r in range(5):
    lines.append("    " + ", ".join('"%s"' % c for c in pal[r*14:(r+1)*14]))
o.append(",\n".join(lines) + "\n  ]\n}\n")
sys.stdout.write("".join(o))
