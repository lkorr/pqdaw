import colorsys, sys
colors = [
 ("surface.background", "#1E1E1E"),
 ("surface.panel", "#2E2E2E"),
 ("surface.raised", "#3A3A3A"),
 ("surface.sunken", "#222222"),
 ("surface.divider", "#141414"),
 ("surface.highlight", "#444444"),
 ("control.fill", "#4A4A4A"),
 ("control.fillHover", "#565656"),
 ("control.fillPressed", "#3D3D3D"),
 ("control.fillDisabled", "#363636"),
 ("control.border", "#151515"),
 ("control.track", "#555555"),
 ("control.value", "#FFA33D"),
 ("control.valueDisabled", "#6A6A6A"),
 ("control.thumb", "#A8A8A8"),
 ("control.thumbHover", "#C6C6C6"),
 ("control.needle", "#E2E2E2"),
 ("field.fill", "#232323"),
 ("field.fillHover", "#292929"),
 ("field.border", "#151515"),
 ("field.borderFocus", "#FFA33D"),
 ("text.primary", "#DCDCDC"),
 ("text.secondary", "#A6A6A6"),
 ("text.disabled", "#808080"),
 ("text.onAccent", "#121212"),
 ("text.selection", "#375E7C"),
 ("text.caret", "#F2F2F2"),
 ("accent.primary", "#FFA33D"),
 ("accent.activator", "#F2C03A"),
 ("accent.solo", "#5DA9E6"),
 ("accent.arm", "#E8544B"),
 ("accent.focus", "#FFA33D"),
 ("selection.fill", "#3D6787"),
 ("selection.text", "#FFFFFF"),
 ("menu.fill", "#333333"),
 ("menu.border", "#151515"),
 ("menu.separator", "#484848"),
 ("menu.shadow", "#00000059"),
 ("tooltip.fill", "#474747"),
 ("tooltip.border", "#151515"),
 ("tooltip.text", "#EEEEEE"),
 ("track.header", "#3A3A3A"),
 ("track.headerSelected", "#4E4E4E"),
 ("track.lane", "#343434"),
 ("track.laneAlt", "#303030"),
 ("track.laneSelected", "#3E3E3E"),
 ("clip.text", "#121212"),
 ("clip.textLight", "#F2F2F2"),
 ("clip.border", "#151515"),
 ("clip.selectedBorder", "#F2F2F2"),
 ("grid.bar", "#00000070"),
 ("grid.beat", "#00000040"),
 ("grid.sub", "#00000024"),
 ("grid.shade", "#00000014"),
 ("scrollbar.track", "#262626"),
 ("scrollbar.thumb", "#5C5C5C"),
 ("scrollbar.thumbHover", "#747474"),
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
 # Roll tokens follow the blue-grey palette of the Live 12 reference screenshot (Q47); the chrome keeps
 # the neutral greys until the retheme.
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
fonts = [("small","regular",12,False),("body","regular",13,False),("bodyStrong","semibold",13,False),("title","semibold",14,False),("numeric","regular",12,False)]
metrics = [("spacing.xs",2),("spacing.sm",4),("spacing.md",8),("spacing.lg",12),
 ("radius.control",2),("radius.panel",3),("radius.menu",3),("radius.clip",2),
 ("size.control",18),("size.row",20),("size.menuItem",20),("size.scrollbar",10),("size.knob",34),("size.meter",14)]

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
o.append('{\n  "name": "Ableton Dark (pqdaw approximation)",\n  "colors": {\n')
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
