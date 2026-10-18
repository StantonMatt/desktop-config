function d(w) { return w.resourceClass + " " + w.frameGeometry.x + "," + w.frameGeometry.y + " " + w.frameGeometry.width + "x" + w.frameGeometry.height; }
workspace.windowAdded.connect(function (w) { print("QROMAP added " + Date.now() + " " + d(w)); });
workspace.windowRemoved.connect(function (w) { print("QROMAP removed " + Date.now() + " " + d(w)); });
