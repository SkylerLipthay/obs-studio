var OBSScene = function(pointer) {
	this.findSource = function(name) {
		var sceneItem = OBS.internal.sceneItemFind(pointer, name);
		return sceneItem ? new OBSSceneItem(sceneItem) : null;
	};

	this.select = function() {
		OBS.internal.sceneSelect(pointer);
	};

	Duktape.fin(this, function() {
		OBS.internal.sceneRelease(pointer);
	});
};

var OBSSceneItem = function(pointer) {
	this.hide = function() {
		this.setVisible(false);
	};

	this.setVisible = function(visible) {
		OBS.internal.sceneItemSetVisible(pointer, !!visible);
	};

	this.show = function() {
		this.setVisible(true);
	};

	Duktape.fin(this, function() {
		OBS.internal.sceneItemRelease(pointer);
	});
};

OBS.findScene = function(name) {
	var scene = OBS.internal.sceneFind(name);
	return scene ? new OBSScene(scene) : null;
};
