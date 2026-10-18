var ws = workspace.windowList();
for (var i = 0; i < ws.length; ++i) { var w = ws[i]; if (w.normalWindow || w.dialog) print("QROWIN " + w.resourceClass + " out=" + (w.output ? w.output.name : "?") + " " + w.frameGeometry.x + "," + w.frameGeometry.y + " " + w.frameGeometry.width + "x" + w.frameGeometry.height + " min=" + w.minimized); }
