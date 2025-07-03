"use strict";

var MarkerLayer;
var NextLon = 0;
var NextLat = 0;

function setup_markers_test() {
    MarkerLayer = new ol.layer.Vector({
        source: new ol.source.Vector(),
    });

    var map = new ol.Map({
        target: 'map_canvas',
        layers: [
            MarkerLayer
        ],
        view: new ol.View({
            center: ol.proj.fromLonLat([5, 0]),
            zoom: 7
        }),
        controls: [new ol.control.Zoom(),
        new ol.control.Rotate()],
        loadTilesWhileAnimating: true,
        loadTilesWhileInteracting: true
    });


    for (var myCategory in MyCategoryIcons) {
        add_marker("Cat " + myCategory, MyCategoryIcons[myCategory], 1);
    }

    for (var type in TypeDesignatorIcons) {
        add_marker(type, TypeDesignatorIcons[type], 0);
    }

    for (var type in TypeDescriptionIcons) {
        add_marker(type, TypeDescriptionIcons[type], 0);
    }

    for (var category in CategoryIcons) {
        add_marker("Cat " + category, CategoryIcons[category], 0);
    }
    add_marker("Default", DefaultIcon);

    map.getView().setCenter(ol.proj.fromLonLat([5, NextLat / 2]));
}

function add_marker(title, baseMarker, ext) {
    var weight = (1 / baseMarker.scale).toFixed(1);

    if (ext === 1) {
        var icon = new ol.style.Icon({
            anchor: baseMarker.anchor,
            anchorXUnits: 'pixels',
            anchorYUnits: 'pixels',
            scale: baseMarker.scale,
            imgSize: baseMarker.size,
            src: svgPathToURI(baseMarker.path, baseMarker.size, '#880000', weight, '#ff0000'),
            rotation: 0,
            opacity: 1.0,
            rotateWithView: (baseMarker.noRotate ? false : true)
        });
    } else {
        var icon = new ol.style.Icon({
            anchor: baseMarker.anchor,
            anchorXUnits: 'pixels',
            anchorYUnits: 'pixels',
            scale: baseMarker.scale,
            imgSize: baseMarker.size,
            src: svgPathToURI(baseMarker.path, baseMarker.size, '#000000', weight, '#00C000'),
            rotation: 0,
            opacity: 1.0,
            rotateWithView: (baseMarker.noRotate ? false : true)
        });

    }
    var markerStyle = new ol.style.Style({
        image: icon,
        text: new ol.style.Text({
            textAlign: 'center',
            textBaseline: 'top',
            offsetY: 30,
            text: title,
            scale: 1.5
        })
    });

    var pos = [NextLon, NextLat];
    var marker = new ol.Feature(new ol.geom.Point(ol.proj.fromLonLat(pos)));
    marker.setStyle(markerStyle);
    MarkerLayer.getSource().addFeature(marker);

    NextLon += 1;
    if (NextLon >= 10) {
        NextLon -= 10;
        NextLat -= 1;
    }
}
