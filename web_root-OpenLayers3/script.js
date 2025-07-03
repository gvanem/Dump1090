// -*- mode: javascript; indent-tabs-mode: nil; c-basic-offset: 8 -*-
"use strict";

// Define our global variables
var OLMap = null;
var StaticFeatures = new ol.Collection();
var SiteCircleFeatures = new ol.Collection();
var PlaneIconFeatures = new ol.Collection();
var PlaneTrailFeatures = new ol.Collection();
var MyFeatures = new ol.Collection();          // AKISSACK Ref: AK9U
var MaxRangeFeatures = new ol.Collection();    // AKISSACK Ref: AK8A
var MidRangeFeatures = new ol.Collection();    // AKISSACK Ref: AK8A
var MinRangeFeatures = new ol.Collection();    // AKISSACK Ref: AK8A
var SleafordRangeFeatures = new ol.Collection(); // AKISSACK Ref: AK8Z
var Planes = {};
var PlanesOrdered = [];
var PlaneFilter = {};
var SelectedPlane = null;
var SelectedAllPlanes = false;
var FollowSelected = false;
var IsDarkMap = false;
var AllwaysShowPermanentLabels = false;
// --------------------------------------------------------------------------------------
// AKISSACK - Variables -----------------------------------------------------------------
// --------------------------------------------------------------------------------------
var acsntext = " ";            // Default label for label data -                Ref: AK7C
var SelectedMilPlanes = false; // Allow selection of all planes of interest     Ref: AK9G
// --------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------- AKISSACK
var SpecialSquawks = {
    '7500': { cssClass: 'squawk7500', markerColor: 'rgb(255, 85, 85)', text: 'Aircraft Hijacking' },
    '7600': { cssClass: 'squawk7600', markerColor: 'rgb(0, 255, 255)', text: 'Radio Failure' },
    '7700': { cssClass: 'squawk7700', markerColor: 'rgb(255, 255, 0)', text: 'General Emergency' }
};

var CenterLat, CenterLon, ZoomLvl, MapType;  // Get current map settings

var Dump1090Version = "unknown version";
var RefreshInterval = 1000;

var PlaneRowTemplate = null;

var TrackedAircraft = 0;
var TrackedAircraftPositions = 0;
var TrackedHistorySize = 0;
var MaxRange = 0;      // AKISSACK Range display Ref: AK9T
var CurMaxRange = 0;   // AKISSACK Range display Ref: AK9T
var CurMinRange = 999; // AKISSACK Range display Ref: AK9T
var MaxRngRange = [];  // AKISSACK Range plot    Ref: AK8B
var MaxRngLat = [];    // AKISSACK Range plot    Ref: AK8B
var MaxRngLon = [];    // AKISSACK Range plot    Ref: AK8B
var MidRngRange = [];  // AKISSACK Range plot    Ref: AK8B
var MidRngLat = [];    // AKISSACK Range plot    Ref: AK8B
var MidRngLon = [];    // AKISSACK Range plot    Ref: AK8B
var MinRngRange = [];  // AKISSACK Range plot    Ref: AK8B
var MinRngLat = [];    // AKISSACK Range plot    Ref: AK8B
var MinRngLon = [];    // AKISSACK Range plot    Ref: AK8B

var lastSidebarWidth = 0; // AKISSACK mapsize    Ref: AKDD

var SitePosition = null;
var ReceiverClock = null;

var LastReceiverTimestamp = 0;
var StaleReceiverCount = 0;
var FetchPending = null;

var MessageCountHistory = [];
var MessageRate = 0;

var NBSP = "\u00a0";

var layers;
var layerGroup;

function checkSidebarWidthChange() {  // AKISSACK mapsize    Ref: AKDD
    var newSidebarWidth = 0;
    newSidebarWidth = $("#sidebar_container").width();
    if (lastSidebarWidth != newSidebarWidth) {
        //console.log("The sidebar was resized");
        updateMapSize();
        lastSidebarWidth = newSidebarWidth;
    }
}

function processReceiverUpdate(data) {
    // Loop through all the planes in the data packet
    var now = data.now;
    var acs = data.aircraft;

    // Detect stats reset
    if (MessageCountHistory.length > 0 && MessageCountHistory[MessageCountHistory.length - 1].messages > data.messages) {
        MessageCountHistory = [
            {
                time: MessageCountHistory[MessageCountHistory.length - 1].time,
                messages: 0,
            },
        ];
    }

    // Note the message count in the history
    MessageCountHistory.push({ time: now, messages: data.messages });
    // .. and clean up any old values
    if (now - MessageCountHistory[0].time > 30) MessageCountHistory.shift();

    for (var j = 0; j < acs.length; j++) {
        var ac = acs[j];
        var hex = ac.hex;
        var squawk = ac.squawk;
        var plane = null;

        // Do we already have this plane object in Planes? If not make it.
        if (Planes[hex]) {
            plane = Planes[hex];
        } else {
            plane = new PlaneObject(hex);
            plane.filter = PlaneFilter;
            plane.tr = PlaneRowTemplate.cloneNode(true);

            if (hex[0] === "~") { // Non-ICAO address
                plane.tr.cells[0].textContent = hex.substring(1);
                $(plane.tr).css("font-style", "italic");
            } else {
                plane.tr.cells[0].textContent = hex;
            }

            // set flag image if available
            if (ShowFlags && plane.icaorange.flag_image !== null) {
                $("img", plane.tr.cells[1]).attr(
                    "src",
                    FlagPath + plane.icaorange.flag_image
                );
                $("img", plane.tr.cells[1]).attr("title", plane.icaorange.country);
            } else {
                $("img", plane.tr.cells[1]).css("display", "none");
            }

            plane.tr.addEventListener(
                "click",
                function (h, evt) {
                    if (evt.srcElement instanceof HTMLAnchorElement) {
                        evt.stopPropagation();
                        return;
                    }
                    if (!$("#map_container").is(":visible")) {
                        showMap();
                    }
                    selectPlaneByHex(h, false);
                    adjustSelectedInfoBlockPosition();
                    evt.preventDefault();
                }.bind(undefined, hex)
            );

            plane.tr.addEventListener(
                "dblclick",
                function (h, evt) {
                    if (!$("#map_container").is(":visible")) {
                        showMap();
                    }
                    selectPlaneByHex(h, true);
                    adjustSelectedInfoBlockPosition();
                    evt.preventDefault();
                }.bind(undefined, hex)
            );

            Planes[hex] = plane;
            PlanesOrdered.push(plane);
        }

        // Call the function update
        plane.updateData(now, ac);
    }
}

function fetchData() {
    if (FetchPending !== null && FetchPending.state() == "pending") {
        // don't double up on fetches, let the last one resolve
        return;
    }

    FetchPending = $.ajax({
        url: EndpointDump1090 + "data/aircraft.json",
        timeout: 5000,
        cache: false,
        dataType: "json",
    });

    FetchPending.done(function (data) {
        var now = data.now;

        processReceiverUpdate(data);

        // update timestamps, visibility, history track for all planes - not only those updated
        for (var i = 0; i < PlanesOrdered.length; ++i) {
            var plane = PlanesOrdered[i];
            plane.updateTick(now, LastReceiverTimestamp);
        }

        selectNewPlanes();
        refreshTableInfo();
        refreshSelected();

        if (ReceiverClock) {
            var rcv = new Date(now * 1000);
            ReceiverClock.render(
                rcv.getUTCHours(),
                rcv.getUTCMinutes(),
                rcv.getUTCSeconds()
            );
        }

        // Check for stale receiver data
        if (LastReceiverTimestamp === now) {
            StaleReceiverCount++;
            if (StaleReceiverCount > 5) {
                $("#update_error_detail").text(
                    "The data from dump1090 hasn't been updated in a while. Maybe dump1090 is no longer running?"
                );
                $("#update_error").css("display", "block");
            }
        } else {
            StaleReceiverCount = 0;
            LastReceiverTimestamp = now;
            $("#update_error").css("display", "none");
        }
    });

    FetchPending.fail(function (jqxhr, status, error) {
        $("#update_error_detail").text(
            "AJAX call failed (" +
            status +
            (error ? ": " + error : "") +
            "). Maybe dump1090 is no longer running?"
        );
        $("#update_error").css("display", "block");
    });
}

var PositionHistorySize = 0;
function initialize() {

    // Set page basics
    document.title = PageName;
    MaxRange = 0; // AKISSACK  Display range  Ref: AK9T

    // $("#infoblock_name").text(PageName); AKISSACK - Ref: AK9W
    $("#infoblock_name").text("");

    PlaneRowTemplate = document.getElementById("plane_row_template");

    $("#timestamps").css("display", "none"); // AKISSACK remove clocks
    $("#loader").removeClass("hidden");

    // Set up map/sidebar splitter
    $("#sidebar_container").resizable({ handles: { w: "#splitter" } });

    // Set up aircraft information panel
    $("#selected_infoblock").draggable({ containment: "parent" });

    // Set up event handlers for buttons
    $("#toggle_sidebar_button").click(toggleSidebarVisibility);
    $("#expand_sidebar_button").click(expandSidebar);
    $("#show_map_button").click(showMap);

    // Set initial element visibility
    $("#show_map_button").hide();
    $("#show_range_admin_buttons").hide();
    setColumnVisibility();

    // Initialize other controls
    initializeUnitsSelector();

    // Set up altitude filter button event handlers and validation options
    $("#altitude_filter_form").submit(onFilterByAltitude);
    $("#altitude_filter_form").validate({
        errorPlacement: function (error, element) {
            return true;
        },
        rules: {
            minAltitude: { number: true, min: -99999, max: 99999, },
            maxAltitude: { number: true, min: -99999, max: 99999, },
        },
    });
    $("#altitude_filter_reset_button").click(onResetAltitudeFilter);

    // Force map to redraw if sidebar container is resized - use a timer to debounce
    var mapResizeTimeout;
    $("#sidebar_container").on("resize", function () {
        clearTimeout(mapResizeTimeout);
        mapResizeTimeout = setTimeout(updateMapSize, 10);
    });

    // Get receiver metadata, reconfigure using it, then continue with initialization
    $.ajax({
        url: EndpointDump1090 + "data/receiver.json",
        timeout: 5000,
        cache: false,
        dataType: "json",
    })

        .done(function (data) {
            if (typeof data.lat !== "undefined") {
                SiteShow = true;
                SiteLat = data.lat;
                SiteLon = data.lon;
                DefaultCenterLat = data.lat;
                DefaultCenterLon = data.lon;
            }

            Dump1090Version = data.version;
            RefreshInterval = data.refresh;
            PositionHistorySize = data.history;
        })

        .always(function () {
            initialize_map();
            start_load_history();
        });

    // AKISSACK Range plot - Now able to read from local (or session) storage if available Ref: AK8C
    if (TypeOfStorageSession == 'Session') {
        if (sessionStorage.getItem("MaxRngLon") && sessionStorage.getItem("MaxRngLat") && sessionStorage.getItem("MaxRngRange")) {
            console.log("Loading max range from session storage");
            MaxRngLat = JSON.parse(sessionStorage.getItem("MaxRngLat"));
            MaxRngLon = JSON.parse(sessionStorage.getItem("MaxRngLon"));
            MaxRngRange = JSON.parse(sessionStorage.getItem("MaxRngRange"));
        } else {
            //console.log("Setting up max range");
            for (var j = 0; j < 720; j++) {
                MaxRngRange[j] = 0;
                MaxRngLat[j] = SiteLat;
                MaxRngLon[j] = SiteLon;
            }
        }

        if (sessionStorage.getItem("MidRngLon") && sessionStorage.getItem("MidRngLat") && sessionStorage.getItem("MidRngRange")) {
            //console.log("Loading mid range");
            MidRngLat = JSON.parse(sessionStorage.getItem("MidRngLat"));
            MidRngLon = JSON.parse(sessionStorage.getItem("MidRngLon"));
            MidRngRange = JSON.parse(sessionStorage.getItem("MidRngRange"));
        } else {
            for (var j = 0; j < 720; j++) {
                MidRngRange[j] = 0;
                MidRngLat[j] = SiteLat;
                MidRngLon[j] = SiteLon;
            }
        }

        if (sessionStorage.getItem("MinRngLon") && sessionStorage.getItem("MinRngLat") && sessionStorage.getItem("MinRngRange")) {
            //console.log("Loading min range");
            MinRngLat = JSON.parse(sessionStorage.getItem("MinRngLat"));
            MinRngLon = JSON.parse(sessionStorage.getItem("MinRngLon"));
            MinRngRange = JSON.parse(sessionStorage.getItem("MinRngRange"));
        } else {
            for (var j = 0; j < 720; j++) {
                MinRngRange[j] = 0;
                MinRngLat[j] = SiteLat;
                MinRngLon[j] = SiteLon;
            }
        }
    } else {
        if (localStorage.getItem("MaxRngLon") && localStorage.getItem("MaxRngLat") && localStorage.getItem("MaxRngRange")) {
            console.log("Loading max range from local storage");
            MaxRngLat = JSON.parse(localStorage.getItem("MaxRngLat"));
            MaxRngLon = JSON.parse(localStorage.getItem("MaxRngLon"));
            MaxRngRange = JSON.parse(localStorage.getItem("MaxRngRange"));
        } else {
            //console.log("Setting up max range");
            for (var j = 0; j < 720; j++) {
                MaxRngRange[j] = 0;
                MaxRngLat[j] = SiteLat;
                MaxRngLon[j] = SiteLon;
            }
        }

        if (localStorage.getItem("MidRngLon") && localStorage.getItem("MidRngLat") && localStorage.getItem("MidRngRange")) {
            //console.log("Loading mid range");
            MidRngLat = JSON.parse(localStorage.getItem("MidRngLat"));
            MidRngLon = JSON.parse(localStorage.getItem("MidRngLon"));
            MidRngRange = JSON.parse(localStorage.getItem("MidRngRange"));
        } else {
            for (var j = 0; j < 720; j++) {
                MidRngRange[j] = 0;
                MidRngLat[j] = SiteLat;
                MidRngLon[j] = SiteLon;
            }
        }

        if (localStorage.getItem("MinRngLon") && localStorage.getItem("MinRngLat") && localStorage.getItem("MinRngRange")) {
            //console.log("Loading min range");
            MinRngLat = JSON.parse(localStorage.getItem("MinRngLat"));
            MinRngLon = JSON.parse(localStorage.getItem("MinRngLon"));
            MinRngRange = JSON.parse(localStorage.getItem("MinRngRange"));
        } else {
            for (var j = 0; j < 720; j++) {
                MinRngRange[j] = 0;
                MinRngLat[j] = SiteLat;
                MinRngLon[j] = SiteLon;
            }
        }
    }
}

var CurrentHistoryFetch = null;
var PositionHistoryBuffer = [];

function start_load_history() {
    if (PositionHistorySize > 0 && window.location.hash != "#nohistory") {
        $("#loader_progress").attr("max", PositionHistorySize);
        console.log("Starting to load history (" + PositionHistorySize + " items)");
        load_history_item(0);
    } else {
        end_load_history();
    }
}

function load_history_item(i) {
    if (i >= PositionHistorySize) {
        end_load_history();
        return;
    }

    // Ref: AK9Y  --  console.log("Loading history #" + i);
    $("#loader_progress").attr("value", i);

    $.ajax({
        url: EndpointDump1090 + "data/history_" + i + ".json",
        timeout: 5000,
        cache: false,
        dataType: "json",
    })

        .done(function (data) {
            PositionHistoryBuffer.push(data);
            load_history_item(i + 1);
        })

        .fail(function (jqxhr, status, error) {
            // No more history
            end_load_history();
        });
}

function end_load_history() {
    $("#loader").addClass("hidden");

    console.log("Done loading history");

    if (PositionHistoryBuffer.length > 0) {
        var now,
            last = 0;

        // Sort history by timestamp
        console.log("Sorting history");
        PositionHistoryBuffer.sort(function (x, y) {
            return x.now - y.now;
        });

        // Process history
        for (var h = 0; h < PositionHistoryBuffer.length; ++h) {
            now = PositionHistoryBuffer[h].now;
            // Ref: AK9Y  --  console.log("Applying history " + h + "/" + PositionHistoryBuffer.length + " at: " + now);
            processReceiverUpdate(PositionHistoryBuffer[h]);

            // update track
            // Ref: AK9Y  --  console.log("Updating tracks at: " + now);
            for (var i = 0; i < PlanesOrdered.length; ++i) {
                var plane = PlanesOrdered[i];
                plane.updateTrack(now - last + 1);
            }
            last = now;
        }

        // Final pass to update all planes to their latest state
        console.log("Final history cleanup pass");
        for (var i = 0; i < PlanesOrdered.length; ++i) {
            var plane = PlanesOrdered[i];
            plane.updateTick(now);
        }
        LastReceiverTimestamp = last;
    }

    PositionHistoryBuffer = null;

    console.log("Completing initialisation.");

    refreshTableInfo();
    refreshSelected();
    reaper();

    // Setup our timer to poll from the server.
    window.setInterval(fetchData, RefreshInterval);
    window.setInterval(reaper, 60000);
    lastSidebarWidth = $("#sidebar_container").width();  // AKISSACK mapsize    Ref: AKDD
    // And kick off one refresh immediately.
    fetchData();
}

// Make a LineString with 'points'- number points
// that is a closed circle on the sphere such that the
// great circle distance from 'center' to each point is
// 'radius' meters
function make_geodesic_circle(center, radius, points) {
    var angularDistance = radius / 6378137.0;
    var lon1 = center[0] * Math.PI / 180.0;
    var lat1 = center[1] * Math.PI / 180.0;
    var geom;
    for (var i = 0; i <= points; ++i) {
        var bearing = i * 2 * Math.PI / points;

        var lat2 = Math.asin(Math.sin(lat1) * Math.cos(angularDistance) +
            Math.cos(lat1) * Math.sin(angularDistance) * Math.cos(bearing));
        var lon2 = lon1 + Math.atan2(Math.sin(bearing) * Math.sin(angularDistance) * Math.cos(lat1),
            Math.cos(angularDistance) - Math.sin(lat1) * Math.sin(lat2));

        lat2 = lat2 * 180.0 / Math.PI;
        lon2 = lon2 * 180.0 / Math.PI;
        if (!geom) {
            geom = new ol.geom.LineString([[lon2, lat2]]);
        } else {
            geom.appendCoordinate([lon2, lat2]);
        }
    }
    return geom;
}

// Initalizes the map and starts up our timers to call various functions
function initialize_map() {
    // Load stored map settings if present
    CenterLat = Number(localStorage["CenterLat"]) ||  DefaultCenterLat;
    CenterLon = Number(localStorage["CenterLon"]) ||  DefaultCenterLon;
    ZoomLvl = Number(localStorage["ZoomLvl"]) ||  DefaultZoomLvl;

    // Set SitePosition, initialize sorting
    if (
        SiteShow &&
        typeof SiteLat !== "undefined" &&
        typeof SiteLon !== "undefined"
    ) {
        SitePosition = [SiteLon, SiteLat];
        sortByDistance();
    } else {
        SitePosition = null;
        PlaneRowTemplate.cells[9].style.display = "none";           // hide distance column
        document.getElementById("distance").style.display = "none"; // hide distance header
        sortByAltitude();
    }

    // Maybe hide flag info
    if (!ShowFlags) {
        PlaneRowTemplate.cells[1].style.display = "none";       // hide flag column
        document.getElementById("flag").style.display = "none"; // hide flag header
        document.getElementById("infoblock_country").style.display = "none"; // hide country row
    }

    // Initialize OL6
    var layers = createBaseLayers();

    // --------------------------------------------------------------
    // AKISSACK - ADD LAYERS ----------------------  ref: AK4A starts
    // --------------------------------------------------------------
 
   // Defining Light Styles
    var aarDayStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({color: "rgba(128,128,255, 0.5)", width: 1,}),
        fill: new ol.style.Fill({color: "rgba(0,0,255, 0.05)", }),
    });
    var tacanDayStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({color: "rgba(0,0,102,0.2)", width: 3, }),
    });
    var matzDayStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(0,0,255, 0.05)", }),
        stroke: new ol.style.Stroke({color: "rgba(128,0,0, 0.5)", width: 0.75, }),
    });
    var ctaDayStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(0, 127,0, 0.03)", }),
        stroke: new ol.style.Stroke({color: "rgba(0,64,0, 0.2)", width: 1, }),
    });
    var airwaysDayStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(0, 102,0, 0.07)", }),
        stroke: new ol.style.Stroke({color: "rgba(0, 64,0, 0.5)", width: 0.2, }),
    });
    var corridorDayStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(102, 0,0, 0.07)", }),
        stroke: new ol.style.Stroke({color: "rgba(255, 0,0, 0.5)", width: 0.2, }),
    });
    var ukDayStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({color: "rgba(0,102,0, 0.2)", width: 3, }),
    });

    // Defining Dark Styles
    var aarNightStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({color: "rgba(128,128,255, 0.5)", width: 1, }),
        fill: new ol.style.Fill({color: "rgba(128,128,255, 0.05)", }),
    });
    var tacanNightStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({color: "rgba(128,128,255,0.2)", width: 3, }),
    });
    var matzNightStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(128,128,255, 0.05)", }),
        stroke: new ol.style.Stroke({color: "rgba(255,128,128, 0.5)", width: 0.75,}),
    });
    var ctaNightStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(128, 255,128, 0.03)", }),
        stroke: new ol.style.Stroke({color: "rgba(128,255,128, 0.2)", width: 1, }),
    });
    var airwaysNightStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(128, 255,128, 0.07)", }),
        stroke: new ol.style.Stroke({color: "rgba(128,255,128, 0.5)", width: 0.2, }),
    });
    var corridorNightStyle = new ol.style.Style({
        fill: new ol.style.Fill({color: "rgba(255,128,128, 0.07)", }),
        stroke: new ol.style.Stroke({color: "rgba(255, 0,0, 0.5)", width: 0.2,}),
    });
    var ukNightStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({color: "rgba(64,255,64, 0.2)", width: 3, }),
    });

    if (ShowUKCivviLayers) {
        var vordmeLayer = new ol.layer.Vector({
            name: "vordme",
            type: "overlay",
            title: "VOR/DME",
            source: new ol.source.Vector({
                url: "layers/UK_VOR+DME+NDB+TACAN.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: (function () {
                var style = new ol.style.Style({
                    image: new ol.style.Icon({
                        src: "layers/img/vor+ndb.png",
                    }),
                    text: new ol.style.Text({
                        text: "field-1",
                        scale: 1,
                        offsetX: 1,
                        offsetY: -11,
                        fill: new ol.style.Fill({
                            color: "#009900",
                        }),
                    }),
                });
                var styles = [style];
                return function (feature, resolution) {
                    style.getText().setText(feature.get("field_2"));
                    return styles;
                };
            })(),
        });
        vordmeLayer.setVisible(false);

        //UK_NavigationPoints.geojson
        var navPointsLayer = new ol.layer.Vector({
            name: "navigation",
            type: "overlay",
            title: "Nav Points",
            source: new ol.source.Vector({
                url: "layers/UK_NavigationPoints.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),

            style: (function () {
                var style = new ol.style.Style({
                    image: new ol.style.Icon({
                        src: "layers/img/point.png",
                    }),
                    text: new ol.style.Text({
                        text: "field-1",
                        scale: 0.75,
                        offsetX: -1,
                        offsetY: 10,
                        fill: new ol.style.Fill({
                            color: "#009900",
                        }),
                    }),
                });

                var styles = [style];
                return function (feature, resolution) {
                    style.getText().setText(feature.get("field_2"));
                    return styles;
                };
            })(),
        });
        navPointsLayer.setVisible(false);

        var airwaysLayer = new ol.layer.Vector({
            name: "airways",
            type: "overlay",
            title: "Airways",
            source: new ol.source.Vector({
                url: "layers/UK_Airways.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: airwaysDayStyle,
        });
        airwaysLayer.setVisible(false);

        var airwaysMRCLayer = new ol.layer.Vector({
            name: "airwaysMRC",
            type: "overlay",
            title: "Radar Corridors",
            source: new ol.source.Vector({
                url: "layers/UK_Mil_RC.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: corridorDayStyle,
        });
        airwaysMRCLayer.setVisible(false);

        var ukCTALayer = new ol.layer.Vector({
            name: "ukcta",
            type: "overlay",
            title: "CTA/TMA",
            source: new ol.source.Vector({
                url: "layers/UK_AT_Areas.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: ctaDayStyle,
        });
        ukCTALayer.setVisible(false);

        var atzLayer = new ol.layer.Vector({
            name: "atz",
            type: "overlay",
            title: "CTR",
            source: new ol.source.Vector({
                url: "layers/UK_ATZ.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: new ol.style.Style({
                fill: new ol.style.Fill({
                    color: "rgba(0,255,0, 0.03)",
                }),
                stroke: new ol.style.Stroke({
                    color: "rgba(0, 192, 0, 0.5)",
                    width: 1,
                }),
            }),
        });

        var airportLayer = new ol.layer.Vector({
            name: "airports",
            type: "overlay",
            title: "Airports",
            source: new ol.source.Vector({
                url: "layers/UK_Civi_Airports_named.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: (function () {
                var style = new ol.style.Style({
                    stroke: new ol.style.Stroke({
                        color: "rgba(0,0,255,1)",
                        width: 1.5,
                    }),
                    text: new ol.style.Text({
                        text: "name",
                        scale: 1,
                        offsetX: 1,
                        offsetY: -10,
                        fill: new ol.style.Fill({
                            color: "#666699",
                        }),
                    }),
                });
                var styles = [style];
                return function (feature, resolution) {
                    if (ShowAirfieldNames && ZoomLvl > 7.5) {
                       style.getText().setText(feature.get("name_1"));
                    }else style.getText().setText("");
                    return styles;
                };
            })(),
        });

        var ukairspaceLayer = new ol.layer.Vector({
            name: "ukair",
            type: "overlay",
            title: "UK Airspace",
            source: new ol.source.Vector({
                url: "layers/UK_Airspace.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: ukDayStyle,
        });
        ukairspaceLayer.setVisible(false);

        var openaip = new ol.layer.Tile({
            // https://docs.openaip.net/?urls.primaryName=Tiles%20API
       	    name: 'openaip',
            title: 'OpenAIP',
            type: 'overlay',
            source: new ol.source.OSM({
               "url" : "https://api.tiles.openaip.net/api/data/openaip/{z}/{x}/{y}.png?apiKey="+OpenAIPAPIKey,
	       // Below caused partial display of tiles
               //"url" : "https://map.adsbexchange.com/mapproxy/tiles/1.0.0/openaip/ul_grid/{z}/{x}/{y}.png",
               "attributions" : "OpenAIP.net",
               attributionsCollapsible: false,
               maxZoom: 12,
               //transition: tileTransition,
               opaque: false,
               opacity: 0.5,
               format: new ol.format.GeoJSON({
                   defaultDataProjection: "EPSG:4326",
                   projection: "EPSG:3857",
               }),
            }),
        });
        openaip.setVisible(false);

     layers.push(
         new ol.layer.Group({
             title: "UK",
             layers: [
          openaip,
                 ukairspaceLayer,
                 navPointsLayer,
                 airwaysMRCLayer,
                 vordmeLayer,
                 airwaysLayer,
                 ukCTALayer,
                 atzLayer,
                 airportLayer,
             ],
         })
     );
 } // End


    // --------------------------------------------------------------
    // AKISSACK - ADD LAYERS ----------------------  ref: AK4A ends
    // --------------------------------------------------------------

    // --------------------------------------------------------------
    // AKISSACK - ADD LAYERS ----------------------  ref: AK5A starts
    // --------------------------------------------------------------

    if (ShowUKMilLayers) {

        // LAYERS for UK Military

        var matzafLayer= new ol.layer.Vector({
            name: "matzaf",
            type: "overlay",
            title: "Airfields",
            source: new ol.source.Vector({
                url: "layers/UK_Mil_Airfield_runways_named.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),

            style: (function () {
                var style = new ol.style.Style({
                    stroke: new ol.style.Stroke({
                        color: "rgba(192,0,0,1)",
                        width: 1.5,
                    }),
                    text: new ol.style.Text({
                        text: "name",
                        scale: 1,
                        offsetX: 1,
                        offsetY: -10,
                        fill: new ol.style.Fill({
                            color: "#666699",
                        }),
                    }),
                });
                var styles = [style];
                return function (feature, resolution) {
                    if (ShowAirfieldNames && ZoomLvl > 7.5) {
                        style.getText().setText(feature.get("name_1"));
                    }else style.getText().setText("");
                    return styles;
                };
            })(),

        });

        var awacLayer = new ol.layer.Vector({
            name: "awac",
            type: "overlay",
            title: "AWACS Zones",
            source: new ol.source.Vector({
                url: "layers/UK_Mil_AWACS_Orbits.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: new ol.style.Style({
                fill: new ol.style.Fill({
                    color: "rgba(0, 255,0, 0.05)",
                }),
                stroke: new ol.style.Stroke({
                    color: "rgba(0, 255,0, 0.5)",
                    width: 0.75,
                }),
            }),
        });
        awacLayer.setVisible(false);

        var dangerLayer = new ol.layer.Vector({
            name: "danger",
            type: "overlay",
            title: "Danger Areas",
            source: new ol.source.Vector({
                url: "layers/UK_Danger_Areas.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: new ol.style.Style({
                fill: new ol.style.Fill({
                    color: "rgba(255, 0,0, 0.05)",
                }),
                stroke: new ol.style.Stroke({
                    color: "rgba(255, 0,0, 0.5)",
                    width: 0.75,
                }),
            }),
        });
        dangerLayer.setVisible(false);

        var AARLayer = new ol.layer.Vector({
            name: "aar",
            type: "overlay",
            title: "AAR Areas",
            source: new ol.source.Vector({
                url: "layers/UK_Mil_AAR_Zones.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: aarDayStyle,
        });
        AARLayer.setVisible(false);

        var matzLayer = new ol.layer.Vector({
            name: "matz",
            type: "overlay",
            title: "MATZ",
            source: new ol.source.Vector({
                url: "layers/UK_MATZ-2023.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: matzDayStyle, 
        });

        var ukmilLayer = new ol.layer.Vector({
            name: "ukmil",
            type: "overlay",
            title: "TACAN Routes",
            source: new ol.source.Vector({
                url: "layers/UK_Military_TACAN_Routes.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: tacanDayStyle,
        });
        ukmilLayer.setVisible(false);

        layers.push(
            new ol.layer.Group({
                title: "UK Military",
                layers: [ukmilLayer, awacLayer, AARLayer, dangerLayer, matzLayer, matzafLayer],
            })
        );
    }

    // --------------------------------------------------------------
    // AKISSACK - ADD LAYERS ----------------------  ref: AK5A ends
    // --------------------------------------------------------------

    var iconsLayer = new ol.layer.Vector({
        name: "ac_positions",
        type: "overlay",
        title: "Aircraft positions",
        source: new ol.source.Vector({
            features: PlaneIconFeatures,
        }),
    });

    if (ShowSleafordRange) {
        // AKISSACK  Ref: AK8Y
        // This is just to show a range ring (based on current experience)
        // for my home QTH. Not usefull for anyone else other than as a technique
        // These points are stored in mySql as seen, and retrieved to draw this

        var SleafordRangeLayer = new ol.layer.Vector({
            name: "mrange",
            type: "overlay",
            title: "Max Seen",
            source: new ol.source.Vector({
                features: SleafordRangeFeatures,
            }),
        });
    } else {
        var SleafordRangeLayer = new ol.layer.Vector({});
    }

    if (ShowSleafordRange) {
        // AKISSACK Ref: AK9T

        // This is just to show a range ring (based on historic experience)
        // for my home QTH. Not usefull for anyone else other than as a technique
        // This is pre-drawn geojson file.

        var rangeLayer = new ol.layer.Vector({
            name: "range",
            type: "overlay",
            title: "Max Range Likely",
            source: new ol.source.Vector({
                url: "layers/AK_range.geojson",
                format: new ol.format.GeoJSON({
                    defaultDataProjection: "EPSG:4326",
                    projection: "EPSG:3857",
                }),
            }),
            style: new ol.style.Style({
                stroke: new ol.style.Stroke({
                    color: "rgba(255,0,0,1)",
                    width: 0.25,
                }),
            }),
        });
    } else {
        var rangeLayer = new ol.layer.Vector({});
    }

    if (ShowRanges) {
        // AKISSACK Maximum, mid and min Range Plots Ref: AK8D
        var maxRangeLayer = new ol.layer.Vector({
            name: "ranges",
            type: "overlay",
            title: "Current Max Range Plot",
            source: new ol.source.Vector({
                features: MaxRangeFeatures,
            }),
        });

        if (MidRangeShow && MidRangeHeight > 0) {
            var midRangeLayer = new ol.layer.Vector({
                name: "ranges2",
                type: "overlay",
                title: "Current Mid Range Plot",
                source: new ol.source.Vector({
                    features: MidRangeFeatures,
                }),
            });
        } else var midRangeLayer = new ol.layer.Vector({});

        if (MinRangeShow && MinRangeHeight > 0) {
            var minRangeLayer = new ol.layer.Vector({
                name: "ranges3",
                type: "overlay",
                title: "Current Min Range Plot",
                source: new ol.source.Vector({
                    features: MinRangeFeatures,
                }),
            });
        } else var minRangeLayer = new ol.layer.Vector({});

    } else {
        var maxRangeLayer = new ol.layer.Vector({});
        var midRangeLayer = new ol.layer.Vector({});
        var minRangeLayer = new ol.layer.Vector({});
    }

    layers.push(
        new ol.layer.Group({
            title: "Overlays",
            layers: [
                new ol.layer.Vector({
                    name: "site_pos",
                    type: "overlay",
                    title: "Site position and range rings",
                    source: new ol.source.Vector({
                        features: StaticFeatures,
                    }),
                }),

                new ol.layer.Vector({
                    name: "ac_trail",
                    type: "overlay",
                    title: "Selected aircraft trail",
                    source: new ol.source.Vector({
                        features: PlaneTrailFeatures,
                    }),
                }),
                SleafordRangeLayer, // Ref: AK8Y
                iconsLayer,
                rangeLayer,
                minRangeLayer,      // Ref: AK8D
                midRangeLayer,      // Ref: AK8D
                maxRangeLayer,      // Ref: AK8D

            ],
        })
    );

    //console.log ("Finds: " + ShowMyFindsLayer +" "+ SleafordMySql);

    if (ShowMyFindsLayer && SleafordMySql) {
        // AKISSACK Ref: AK9U
        var myLayer = new ol.layer.Vector({
            name: "my_layer",
            type: "overlay",
            title: "Finds",
            source: new ol.source.Vector({
                features: MyFeatures,
            }),
        });

        layers.push(
            new ol.layer.Group({
                title: "Private",
                layers: [myLayer],
            })
        );
    }

    MapType = localStorage["MapType"];
    if (MapType === undefined) {
      console.log("MapType local is " + MapType);
      MapType = "osm_light";
    }
    console.log("MapType is " + MapType);

    var foundType = false;
    var baseCount = 0;

    layerGroup = new ol.layer.Group({
        layers: layers
    })

    ol.control.LayerSwitcher.forEachRecursive(layerGroup, function (lyr) {
        if (!lyr.get("name")) {
            return;
        }
 
        if (lyr.get("type") === "base") {
            baseCount++;
            //console.log("DEBUG BASE " + baseCount + " - " + lyr.get("type") + " - " + lyr.get("name"));
            //console.log("DEBUG MapType " + MapType);

            if (MapType === lyr.get("name")) {
                foundType = true;
                lyr.setVisible(true);
                baseLayerChange(MapType);
                //console.log("DEBUG MapType " + baseCount + " - " + lyr.get("type") + " - " + lyr.get("name"));
            } else {
                lyr.setVisible(false);
            }

            lyr.on("change:visible", function (evt) {
                if (evt.target.getVisible()) {
                    MapType = localStorage["MapType"] = evt.target.get("name");
                    createSiteCircleFeatures();
                    baseLayerChange(evt.target.get("name"));
                }
            });
        } else if (lyr.get("type") === "overlay") {
            var visible = localStorage["layer_" + lyr.get("name")];
            if (visible != undefined) {
                // javascript, why must you taunt me with gratuitous type problems
                lyr.setVisible(visible === "true");
            }

            lyr.on("change:visible", function (evt) {
                localStorage["layer_" + evt.target.get("name")] =
                    evt.target.getVisible();
            });
        }
    });

    if (!foundType) {
        ol.control.LayerSwitcher.forEachRecursive(layers, function (lyr) {
            if (foundType) 
                return;
            if (lyr.get("type") === "base") {
                lyr.setVisible(true);
                foundType = true;
            }
        });
    }

    OLMap = new ol.Map({
        target: "map_canvas",
        layers: layers,

        view: new ol.View({
            center: ol.proj.fromLonLat([CenterLon, CenterLat]),
            zoom: ZoomLvl
        }),
        controls: [
            new ol.control.Zoom(),
            new ol.control.Rotate(),
            new ol.control.Attribution({ collapsed: true }),
            new ol.control.ScaleLine({ units: DisplayUnits }),
            //new ol.control.LayerSwitcher(),
        ],
        loadTilesWhileAnimating: true,
        loadTilesWhileInteracting: true,
    });

    if (baseCount > 1) {
        OLMap.addControl(new ol.control.LayerSwitcher());
    }

    // Listeners for newly created Map
    OLMap.getView().on("change:center", function (event) {
        var center = ol.proj.toLonLat(
            OLMap.getView().getCenter(),
            OLMap.getView().getProjection()
        );
        localStorage["CenterLon"] = center[0];
        localStorage["CenterLat"] = center[1];
        if (FollowSelected) {
            // On manual navigation, disable follow
            var selected = Planes[SelectedPlane];
            if (
                Math.abs(center[0] - selected.position[0]) > 0.0001 &&
                Math.abs(center[1] - selected.position[1]) > 0.0001
            ) {
                FollowSelected = false;
                refreshSelected();
            }
        }
    });

    OLMap.getView().on("change:resolution", function (event) {
        ZoomLvl = localStorage["ZoomLvl"] = OLMap.getView().getZoom();
        for (var plane in Planes) {
            Planes[plane].updateMarker(false);
        }
    });

    var hitTolerance = 5;
    OLMap.on(["click", "dblclick"], function (evt) {
        var hex = evt.map.forEachFeatureAtPixel(
            evt.pixel,
            function (feature, layer) {
                return feature.hex;
            },
            {
                hitTolerance: hitTolerance,
            },
            null,
            function (layer) {
                return layer === iconsLayer;
            },
            null
        );
        if (hex) {
            selectPlaneByHex(hex, evt.type === "dblclick");
            adjustSelectedInfoBlockPosition();
            evt.stopPropagation();
        } else {
            deselectAllPlanes();
            evt.stopPropagation();
        }
    });

    //------------------------------------------------------------------------------------
    // AKISSACK - MOUSE POSITION ----------------------------------- ---- Ref: AK1C starts
    //------------------------------------------------------------------------------------
    var llFormat = function (dgts) {
        return function (coord1) {
            var coord2 = [coord1[1], coord1[0]];
            // AKISSACK - also add range and bearing if site is known --  Ref: AK1D
            var akret = ol.coordinate.toStringXY(coord2, dgts);
            if (SitePosition !== null) {
                var akbrn = parseInt(
                    getBearing(SitePosition[1], SitePosition[0], coord1[1], coord1[0])
                ).toString();
                var akrng = ol.sphere.getDistance(SitePosition, coord1);
                return (
                    akret + " " + akbrn + "\u00B0 " +
                    format_distance_long(akrng, DisplayUnits, 0) //+ " "+ZoomLvl
                );
            } else {
                return akret; // no range or bearing required, just return akret
            }
        };
    };

    var mousePosition = new ol.control.MousePosition({
        coordinateFormat: llFormat(3), 
        projection: "EPSG:4326",
        target: document.getElementById("mouseposition"),
        undefinedHTML: "&nbsp;",
    });

    if (ShowMouseLatLong) OLMap.addControl(mousePosition);
    //------------------------------------------------------------------------------------
    // Ref: AK1C Ends ----------------------------------------------------------- AKISSACK
    //------------------------------------------------------------------------------------

    //------------------------------------------------------------------------------------
    // // AKISSACK Ref: AK8X -------------------------------------------------------------
    //------------------------------------------------------------------------------------
    // Read the stored maximum range (lat/long) from my mySql database and then plot these
    // as a polygon.  This will be update as positions are logged and will therefore become more
    // accurate, although rouge spikes will need to be manually removed from the database
    // Expanded to include the 2 other rings too, if required

    var polyCoordsMax = [];
    var polyCoordsMid = [];
    var polyCoordsMin = [];

    $(function () {
        $.ajax({
            //url: 'sql/range_read.php',
            url: "sql/range_read.php",
            data: "",
            dataType: "json",
            success: function (data) {
                processMrData(data);
            },
        });
    });

    function processMrData(returnedData) {
        for (var i in returnedData) {
            var oneRPoint = returnedData[i];

            //for(var y in oneRPoint ) {
            //	if(y == "minlat") {         var minlat = oneRPoint[y];
            //	} else if (y == "minlon") { var minlon = oneRPoint[y];
            //	} else if (y == "midlat") { var midlat = oneRPoint[y];
            //	} else if (y == "midlon") { var midlon = oneRPoint[y];
            //	} else if (y == "maxlat") { var maxlat = oneRPoint[y];
            //	} else if (y == "maxlon") { var maxlon = oneRPoint[y];
            //	}
            //}
            //polyCoordsMax.push(ol.proj.transform([parseFloat(maxlon), parseFloat(maxlat)], 'EPSG:4326', 'EPSG:3857'));
            //polyCoordsMid.push(ol.proj.transform([parseFloat(midlon), parseFloat(midlat)], 'EPSG:4326', 'EPSG:3857'));
            //polyCoordsMin.push(ol.proj.transform([parseFloat(minlon), parseFloat(minlat)], 'EPSG:4326', 'EPSG:3857'));

            for (var y in oneRPoint) {
                if (y == "minlat") {
                    var minlat = oneRPoint[y];
                } else if (y == "minlon") {
                    var minlon = oneRPoint[y];
                } else if (y == "midlat") {
                    var midlat = oneRPoint[y];
                } else if (y == "midlon") {
                    var midlon = oneRPoint[y];
                } else if (y == "maxlat") {
                    var maxlat = oneRPoint[y];
                } else if (y == "maxlon") {
                    var maxlon = oneRPoint[y];
                }
            }
            polyCoordsMax.push(
                ol.proj.transform(
                    [parseFloat(maxlon), parseFloat(maxlat)],
                    "EPSG:4326",
                    "EPSG:3857"
                )
            );
            polyCoordsMid.push(
                ol.proj.transform(
                    [parseFloat(midlon), parseFloat(midlat)],
                    "EPSG:4326",
                    "EPSG:3857"
                )
            );
            polyCoordsMin.push(
                ol.proj.transform(
                    [parseFloat(minlon), parseFloat(minlat)],
                    "EPSG:4326",
                    "EPSG:3857"
                )
            );
            //polyCoordsMid.push(ol.proj.transform([parseFloat(midlon), parseFloat(midlat)], 'EPSG:4326', 'EPSG:3857'));
            //polyCoordsMin.push(ol.proj.transform([parseFloat(minlon), parseFloat(minlat)], 'EPSG:4326', 'EPSG:3857'));
        }

        // Max range we'll always show
        var styleMax = new ol.style.Style({
            stroke: new ol.style.Stroke({
                lineDash: [2, 4],
                color: "rgba(0,0,64, 2)",
                width: 0.5,
            }),
            //fill: new ol.style.Fill({
            //  color: "rgba(0,0,255, 0.07)",
            //}),
        });

        var rfeatureMax = new ol.Feature({
            geometry: new ol.geom.Polygon([polyCoordsMax]),
        });

        rfeatureMax.setStyle(styleMax);
        SleafordRangeFeatures.push(rfeatureMax);

        // mid range only if user has set height constant
        if (MidRangeShow && MidRangeHeight > 0) {
            var styleMid = new ol.style.Style({
                stroke: new ol.style.Stroke({
                    lineDash: [2, 4],
                    color: "rgba(0,64,0, 2)",
                    width: 0.5,
                }),
                fill: new ol.style.Fill({
                    color: "rgba(0,255,0, 0.07)",
                }),
            });

            var rfeatureMid = new ol.Feature({
                geometry: new ol.geom.Polygon([polyCoordsMid]),
            });
            rfeatureMid.setStyle(styleMid);
            SleafordRangeFeatures.push(rfeatureMid);
        }

        // minimum range only if user has set height constant
        if (MinRangeShow && MinRangeHeight > 0) {
            var styleMin = new ol.style.Style({
                stroke: new ol.style.Stroke({
                    lineDash: [2, 4],
                    color: "rgba(64,0,0, 2)",
                    width: 0.5,
                }),
                fill: new ol.style.Fill({
                    color: "rgba(255,0,0, 0.07)",
                }),
            });

            var rfeatureMin = new ol.Feature({
                geometry: new ol.geom.Polygon([polyCoordsMin]),
            });
            rfeatureMin.setStyle(styleMin);
            SleafordRangeFeatures.push(rfeatureMin);
        }
    }
    //------------------------------------------------------------------------------------
    // Ref: AK8X Ends ----------------------------------------------------------- AKISSACK
    //------------------------------------------------------------------------------------

    //------------------------------------------------------------------------------------
    // // AKISSACK Ref: AK9U -------------------------------------------------------------
    //------------------------------------------------------------------------------------
    // This section can be ignored.  It is just a test to show my metal detecting finds
    //------------------------------------------------------------------------------------
    if (ShowMyFindsLayer && SleafordMySql) {
        // AKISSACK Ref: AK9U

        var fCoin = new ol.style.Style({
            image: new ol.style.Icon({
                src: "sql/img/coin.png",
            }),
        });
        var fCoins = new ol.style.Style({
            image: new ol.style.Icon({
                src: "sql/img/coins.png",
            }),
        });
        var fCoinr = new ol.style.Style({
            image: new ol.style.Icon({
                src: "sql/img/coinr.png",
            }),
        });

        var fSpade = new ol.style.Style({
            image: new ol.style.Icon({
                src: "sql/img/spade.png",
            }),
        });
        var fSpader = new ol.style.Style({
            image: new ol.style.Icon({
                src: "sql/img/spader.png",
            }),
        });
        var fSpec = new ol.style.Style({
            image: new ol.style.Icon({
                src: "sql/img/spec.png",
            }),
        });
        var fMil = new ol.style.Style({
            image: new ol.style.Icon({
                src: "sql/img/mil.png",
            }),
        });

        $(
            function ()
            //-----------------------------------------------------------------------
            // Send a http request with AJAX http://api.jquery.com/jQuery.ajax/
            // install: apt-get install mysql-client php5-mysql
            //-----------------------------------------------------------------------
            {
                $.ajax({
                    url: "sql/sql-finds-layer.php",
                    data: "",
                    dataType: "json",
                    success: function (data) {
                        processMdData(data);
                    },
                });
            }
        );

        function processMdData(allFindData) {
            // New SQL Database etc Feb 2022
            //console.log(allFindData);
            for (var i in allFindData) {
                var oneFind = allFindData[i];
                for (var y in oneFind) {
                    // Get elements of JSON array
                    if (y == "Lat") {
                        var findlat = oneFind[y];
                    } else if (y == "Long") {
                        var findlon = oneFind[y];
                    } else if (y == "Name") {
                        var findname = oneFind[y];
                    } else if (y == "Number") {
                        var findnumber = oneFind[y];
                    } else if (y == "icon") {
                        var findicon = oneFind[y];
                    } else if (y == "desc") {
                        var finddesc = oneFind[y];
                    } else if (y == "Score") {
                        var findscore = oneFind[y];
                    }
                    var f = new ol.Feature({
                        geometry: new ol.geom.Point(
                            ol.proj.transform([+findlon, +findlat], "EPSG:4326", "EPSG:3857")
                        ),
                        name: findname + "<br>" + findnumber,
                    });

		    //Simpler iconisation
		    if (findname.startsWith("Coin")){
	                f.setStyle(fCoin);
		    //}

                    //if (findicon === "coin") {
                    //    f.setStyle(fCoin);
                    //} else if (findicon === "coins") {
                    //    f.setStyle(fCoins);
                    //} else if (findicon === "coinr") {
                    //    f.setStyle(fCoinr);
                    //} else if (findicon === "mil") {
                    //    f.setStyle(fMil);
                    //} else if (findicon === "spec") {
                    //    f.setStyle(fSpec);
                    //} else if (findicon === "spader") {
                    //    f.setStyle(fSpader);
                    } else {
                        f.setStyle(fSpade);
                    }
                }
                MyFeatures.push(f);
            } // end of i in all find data
        }
        //------------------------------------------------------------------------------------
        // // AKISSACK Ref: AK9U ---------------------------------------------------- END
        //------------------------------------------------------------------------------------
    }

    //------------------------------------------------------------------------------------
    // AKISSACK - HOVER OVER LABELS ------------------------------------- ref: AK6D starts
    //------------------------------------------------------------------------------------
    if (ShowHoverOverLabels) {
        var overlay = new ol.Overlay({
            element: document.getElementById("popinfo"),
            positioning: "bottom-left",
        });
        overlay.setMap(OLMap);

        // trap mouse moving over
        var hitTolerance = 5;
        OLMap.on("pointermove", function (evt) {
            var feature = OLMap.forEachFeatureAtPixel(
                evt.pixel,
                function (feature, layer) {
                    overlay.setPosition(evt.coordinate);
                    var popname = feature.get("name");
                    //console.log("popname: " + popname);

                    if (
                        ShowMyFindsLayer &&
                        typeof popname != "undefined" &&
                        popname != "~"
                    ) {
                        overlay.getElement().innerHTML = popname ? popname : "";
                        return feature;
                    }

                    if (popname === "~") {
                        var vsi = "";
                        if (Planes[feature.hex].vert_rate !== "undefined") {
                            // Correct odd errors
                            if (Planes[feature.hex].vert_rate > 256) {
                                vsi = "climbing";
                            } else {
                                if (Planes[feature.hex].vert_rate < -256) {
                                    vsi = "descending";
                                } else vsi = "level";
                            }
                        }

                        if (ShowAdditionalData) {
                            //LINE ONE
                            popname = Planes[feature.hex].ac_aircraft
                                ? Planes[feature.hex].ac_aircraft
                                : "-";
                            if (popname === "-") {
                                //  Let's try an alternative to ID -> https://github.com/alkissack/Dump1090-OpenLayers3-html/issues/3
                                popname = Planes[feature.hex].icaotype
                                    ? Planes[feature.hex].icaotype
                                    : "Unknown aircraft type";
                            }
                            popname =
                                popname +
                                " [" +
                                (Planes[feature.hex].category
                                    ? Planes[feature.hex].category
                                    : "?") +
                                "]";
                            //LINE TWO
                            popname =
                                popname +
                                "\n(" +
                                (Planes[feature.hex].flight
                                    ? Planes[feature.hex].flight.trim()
                                    : "No Call") +
                                ")";
                            popname = popname + " #" + feature.hex.toUpperCase();
                            popname = popname + " [" + Planes[feature.hex].registration + "]";

                            //LINE THREE
                            var hgt = parseInt(Planes[feature.hex].altitude ? (Planes[feature.hex].altitude) : 0);
                            hgt = convert_altitude(hgt, DisplayUnits);
                            //console.log("Height.. " + hgt);
                            popname =
                                popname +
                                "\n" + parseInt(hgt) + (DisplayUnits === "metric" ? "m & " : " ft & ") + vsi;

                            //LINE FOUR
                            popname =
                                popname +
                                "\n" +
                                (Planes[feature.hex].country
                                    ? Planes[feature.hex].country
                                    : "");
                            popname =
                                popname +
                                " " +
                                (Planes[feature.hex].operator
                                    ? Planes[feature.hex].operator
                                    : "");

                            var dst = parseInt(Planes[feature.hex].siteNm ? (Planes[feature.hex].siteNm) : 0);
                            dst = convert_nm_distance(dst, DisplayUnits);
                            //console.log("Distance.. " + dst);
                            popname =
                                popname +
                                " " + dst.toFixed(2) + (DisplayUnits === "metric" ? " km " : DisplayUnits === "imperial" ? " mile " : " nm ");

                            popname =
                                popname +
                                " " +
                                (Planes[feature.hex].siteBearing
                                    ? Planes[feature.hex].siteBearing + "\u00B0"
                                    : "");

                        } else {
                            popname = "ICAO: " + Planes[feature.hex].icao;
                            popname =
                                popname +
                                "\nFlt:  " +
                                (Planes[feature.hex].flight ? Planes[feature.hex].flight : "?");
                            popname =
                                popname +
                                "\nType: " +
                                (Planes[feature.hex].icaotype
                                    ? Planes[feature.hex].icaotype
                                    : "?");
                            popname =
                                popname +
                                "\nReg:  " +
                                (Planes[feature.hex].registration
                                    ? Planes[feature.hex].registration
                                    : "?");

                            var hgt = parseInt(Planes[feature.hex].altitude ? (Planes[feature.hex].altitude) : 0);
                            hgt = convert_altitude(hgt, DisplayUnits);
                            //console.log("Height.. " + hgt);
                            popname =
                                popname +
                                "\nAlt:  " + parseInt(hgt) + (DisplayUnits === "metric" ? "m " : " ft ");

                        }
                        overlay.getElement().innerHTML = popname ? popname : "";
                        return feature;
                    } else {
                        //overlay.getElement().innerHTML = (popname  ?  popname   :'' );
                        //return feature;
                        return null;
                    }
                },
                {
                    hitTolerance: hitTolerance,
                },
                null,
                function (layer) {
                    if (ShowMyFindsLayer) {
                        return layer == iconsLayer, MyFeatures;
                    } else {
                        return layer == iconsLayer;
                    }
                }
            ); //OLMap.forEachFeatureAtPixel

            overlay.getElement().style.display = feature ? "" : "none"; // EAK--> Needs GMAP/INDEX.HTML
            document.body.style.cursor = feature ? "pointer" : "";
        });
    } else {
        // Labels are not required
        var overlay = new ol.Overlay({
            element: document.getElementById("popinfo"),
            positioning: "bottom-left",
        });
        overlay.setMap(OLMap);
    }
    //------------------------------------------------------------------------------------
    // -------------------------------------------------------------------- ref: AK6D ends
    //------------------------------------------------------------------------------------

    // Add home marker if requested
    if (SitePosition) {
        if (ShowMyPreferences) {
            // Personal preferences Ref: AK9V
            var homeRad = 2;
            var homeWid = 1;
        } else {
            var homeRad = 7;
            var homeWid = 2;
        }
        var markerStyle = new ol.style.Style({
            image: new ol.style.Circle({
                radius: homeRad, // Ref: AK9V
                snapToPixel: false,
                fill: new ol.style.Fill({ color: "black" }),
                stroke: new ol.style.Stroke({
                    color: "white",
                    width: homeWid, // Ref: AK9V
                }),
            }),
        });

        var feature = new ol.Feature(
            new ol.geom.Point(ol.proj.fromLonLat(SitePosition))
        );
        feature.setStyle(markerStyle);
        StaticFeatures.push(feature);

        if (SiteCircles) {
            createSiteCircleFeatures();
        }
    }

    // Add terrain-limit rings. To enable this:
    //
    //  create a panorama for your receiver location on heywhatsthat.com
    //
    //  note the "view" value from the URL at the top of the panorama
    //    i.e. the XXXX in http://www.heywhatsthat.com/?view=XXXX
    //
    // fetch a json file from the API for the altitudes you want to see:
    //
    //  wget -O /usr/share/dump1090-mutability/html/upintheair.json \
    //    'http://www.heywhatsthat.com/api/upintheair.json?id=XXXX&refraction=0.25&alts=3048,9144'
    //
    // NB: altitudes are in _meters_, you can specify a list of altitudes

    // kick off an ajax request that will add the rings when it's done
    var request = $.ajax({
        url: 'upintheair.json',
        timeout: 5000,
        cache: true,
        dataType: 'json'
    });
    request.done(function (data) {
        for (var i = 0; i < data.rings.length; ++i) {
            var geom = new ol.geom.LineString([]);
            var points = data.rings[i].points;
            if (points.length > 0) {
                for (var j = 0; j < points.length; ++j) {
                    geom.appendCoordinate([points[j][1], points[j][0]]);
                }
                geom.appendCoordinate([points[0][1], points[0][0]]);
                geom.transform('EPSG:4326', 'EPSG:3857');

                var feature = new ol.Feature(geom);
                feature.setStyle(ringStyleForAlt(data.rings[i].alt));
                StaticFeatures.push(feature);
            }
        }
    });

    request.fail(function (jqxhr, status, error) {
        // no rings available, do nothing
    });

    function baseLayerChange(n) {
        //console.log("DEBUG - layer change " + n);
        if(n === "carto_dark_nolabels") {
            //console.log("DEBUG - dark");
            IsDarkMap = true;
            // Style changes
            AARLayer.setStyle(aarNightStyle);
            ukmilLayer.setStyle(tacanNightStyle);
            matzLayer.setStyle(matzNightStyle); 
            ukCTALayer.setStyle(ctaNightStyle);
            airwaysLayer.setStyle(airwaysNightStyle);
            airwaysMRCLayer.setStyle(corridorNightStyle);
            ukairspaceLayer.setStyle(ukNightStyle);
            const elementInfo = document.querySelector('#selected_infoblock');
            elementInfo.style.background = '#888888';
            const elementCanvas = document.querySelector('body');
            elementCanvas.style.backgroundColor= '#444444';
            const elementSide = document.querySelector('#sidebar_container');
            elementSide.style.color= '#ffffff';
            const elementMouse = document.querySelector('div#mouseposition');
            elementMouse.style.color= '#ffffff';
        } else {
            //console.log("DEBUG - light");
            IsDarkMap = false;
            AARLayer.setStyle(aarDayStyle);
            ukmilLayer.setStyle(tacanDayStyle);
            matzLayer.setStyle(matzDayStyle); 
            ukCTALayer.setStyle(ctaDayStyle);
            airwaysLayer.setStyle(airwaysDayStyle);
            airwaysMRCLayer.setStyle(corridorDayStyle);
            ukairspaceLayer.setStyle(ukDayStyle);
            const elementInfo = document.querySelector('#selected_infoblock');
            elementInfo.style.background = '#ffffff';
            const elementCanvas= document.querySelector('body');
            elementCanvas.style.backgroundColor= '#ffffff';
            const elementSide = document.querySelector('#sidebar_container');
            elementSide.style.color= '#000000';
            const elementMouse = document.querySelector('div#mouseposition');
            elementMouse.style.color= 'blue';

        }
    }
}

function ringStyleForAlt(altitude) {
    return new ol.style.Style({
        fill: null,
        stroke: new ol.style.Stroke({
            color: "#aa0000",
            lineDash: UseTerrainLineDash ? [4, 4] : null,
            width: TerrainLineWidth,
            //color: PlaneObject.prototype.hslRepr(PlaneObject.prototype.getAltitudeColor(altitude*3.281)), // converting from m to ft
            //width: 1
        })
    });
}

function createSiteCircleFeatures() {
    // Clear existing circles first
    SiteCircleFeatures.forEach(function (circleFeature) {
        StaticFeatures.remove(circleFeature);
    });
    SiteCircleFeatures.clear();
    if (ShowMyPreferences) {
        // Personal preferences Ref: AK9V
        var rangeWid = 0.25;
    } else {
        var rangeWid = 1;
    }
    var circleStyle = function (distance) {
        return new ol.style.Style({
            fill: null,
            stroke: new ol.style.Stroke({
                color: "#000000",
                width: rangeWid, //
            }),
            text: new ol.style.Text({
                font: "bold 10px Helvetica Neue, sans-serif",
                fill: new ol.style.Fill({ color: "#000000" }),
                offsetY: -8,
                offsetX: 1,
                text: ShowSiteRingDistanceText
                    ? format_distance_long(distance, DisplayUnits, 0)
                    : "",
            }),
        });
    };

    var conversionFactor = 1000.0;
    if (DisplayUnits === "nautical") {
        conversionFactor = 1852.0;
    } else if (DisplayUnits === "imperial") {
        conversionFactor = 1609.0;
    }

    for (var i = 0; i < SiteCirclesDistances.length; ++i) {
        var distance = SiteCirclesDistances[i] * conversionFactor;
        var circle = make_geodesic_circle(SitePosition, distance, 360);
        circle.transform("EPSG:4326", "EPSG:3857");
        var feature = new ol.Feature(circle);
        feature.setStyle(circleStyle(distance));
        StaticFeatures.push(feature);
        SiteCircleFeatures.push(feature);
    }
}

// This looks for planes to reap out of the master Planes variable
function reaper() {
    //console.log("Reaping started..");

    // Look for planes where we have seen no messages for >300 seconds
    var newPlanes = [];
    for (var i = 0; i < PlanesOrdered.length; ++i) {
        var plane = PlanesOrdered[i];
        if (plane.seen > 300) {
            // Reap it.
            plane.tr.parentNode.removeChild(plane.tr);
            plane.tr = null;
            delete Planes[plane.icao];
            plane.destroy();
        } else {
            // Keep it.
            newPlanes.push(plane);
        }
    }

    PlanesOrdered = newPlanes;
    refreshTableInfo();
    refreshSelected();
}

// Page Title update function
function refreshPageTitle() {
    if (!PlaneCountInTitle && !MessageRateInTitle) return;

    var subtitle = "";

    if (PlaneCountInTitle) {
        // AKISSACK add Max' Range  AK9T
        subtitle +=
            format_distance_brief(CurMinRange, DisplayUnits) +
            "-" +
            format_distance_brief(CurMaxRange, DisplayUnits) +
            ">";
        subtitle += TrackedAircraftPositions + "/" + TrackedAircraft;
    }

    if (MessageRateInTitle) {
        if (subtitle) subtitle += " | ";
        subtitle += MessageRate.toFixed(1) + "/s";
    }

    //document.title = PageName + ' - ' + subtitle;  // AKISSACK Ref: AK9X
    document.title = subtitle + " " + PageName; // AKISSACK Ref: AK9X
}

// Refresh the detail window about the plane
function refreshSelected() {
    if (MessageCountHistory.length > 1) {
        var message_time_delta =
            MessageCountHistory[MessageCountHistory.length - 1].time -
            MessageCountHistory[0].time;
        var message_count_delta =
            MessageCountHistory[MessageCountHistory.length - 1].messages -
            MessageCountHistory[0].messages;
        if (message_time_delta > 0)
            MessageRate = message_count_delta / message_time_delta;
    } else {
        MessageRate = null;
    }

    refreshPageTitle();

    var selected = false;
    if (
        typeof SelectedPlane !== "undefined" &&
        SelectedPlane != "ICAO" &&
        SelectedPlane != null
    ) {
        selected = Planes[SelectedPlane];
    }

    $("#dump1090_infoblock").css("display", "block");
    //$('#dump1090_version').text(Dump1090Version);     AKISSACK Ref: AK9W
    $("#dump1090_version").text(""); // AKISSACK Ref: AK9W
    $("#dump1090_total_ac").text(TrackedAircraft);
    $("#dump1090_total_ac_positions").text(TrackedAircraftPositions);
    $("#dump1090_max_range").text(format_distance_brief(MaxRange, DisplayUnits)); // Ref: AK9T
    $("#dump1090_total_history").text(TrackedHistorySize);

    if (MessageRate !== null) {
        $("#dump1090_message_rate").text(MessageRate.toFixed(1));
    } else {
        $("#dump1090_message_rate").text("n/a");
    }

    setSelectedInfoBlockVisibility();

    if (!selected) {
        return;
    }

    if (selected.flight !== null && selected.flight !== "") {
        $("#selected_callsign").text(selected.flight);
    } else {
        $("#selected_callsign").text("n/a");
    }
    $("#selected_flightaware_link").html(
        getFlightAwareModeSLink(selected.icao, selected.flight, "[FlightAware]")
    );

    if (selected.registration !== null) {
        $("#selected_registration").text(selected.registration);
    } else {
        $("#selected_registration").text("");
    }

    if (selected.icaotype !== null) {
        $("#selected_icaotype").text(selected.icaotype);
    } else {
        $("#selected_icaotype").text("");
    }

    var emerg = document.getElementById("selected_emergency");
    if (selected.squawk in SpecialSquawks) {
        emerg.className = SpecialSquawks[selected.squawk].cssClass;
        emerg.textContent = NBSP + "Squawking: " + SpecialSquawks[selected.squawk].text + NBSP;
    } else {
        emerg.className = "hidden";
    }

    $("#selected_altitude").text(
        format_altitude_long(selected.altitude, selected.vert_rate, DisplayUnits)
    );

    if (selected.squawk === null || selected.squawk === "0000") {
        $("#selected_squawk").text("n/a");
    } else {
        $("#selected_squawk").text(selected.squawk);
    }

    $("#selected_speed").text(format_speed_long(selected.speed, DisplayUnits));
    $("#selected_vertical_rate").text(
        format_vert_rate_long(selected.vert_rate, DisplayUnits)
    );
    $("#selected_icao").text(selected.icao.toUpperCase());
    $("#airframes_post_icao").attr("value", selected.icao);
    $("#selected_track").text(format_track_long(selected.track));

    if (selected.seen <= 1) {
        $("#selected_seen").text("now");
    } else {
        $("#selected_seen").text(selected.seen.toFixed(1) + "s");
    }

    $("#selected_country").text(selected.icaorange.country);
    if (ShowFlags && selected.icaorange.flag_image !== null) {
        $("#selected_flag").removeClass("hidden");
        $("#selected_flag img").attr(
            "src",
            FlagPath + selected.icaorange.flag_image
        );
        $("#selected_flag img").attr("title", selected.icaorange.country);
    } else {
        $("#selected_flag").addClass("hidden");
    }

    if (selected.position === null) {
        $("#selected_position").text("n/a");
        $("#selected_follow").addClass("hidden");
    } else {
        var mlat_bit = selected.position_from_mlat ? "MLAT: " : "";
        if (selected.seen_pos > 1) {
            $("#selected_position").text(
                mlat_bit +
                format_latlng(selected.position) +
                " (" +
                selected.seen_pos.toFixed(1) +
                "s)"
            );
        } else {
            $("#selected_position").text(mlat_bit + format_latlng(selected.position));
        }
        $("#selected_follow").removeClass("hidden");
        if (FollowSelected) {
            $("#selected_follow").css("font-weight", "bold");
            OLMap.getView().setCenter(ol.proj.fromLonLat(selected.position));
        } else {
            $("#selected_follow").css("font-weight", "normal");
        }
    }

    $("#selected_sitedist").text(
        format_distance_long(selected.sitedist, DisplayUnits)
    );
    $("#selected_rssi").text(selected.rssi.toFixed(1) + " dBFS");
    $("#selected_message_count").text(selected.messages);
    if (UseJetPhotosPhotoLink) {
        $("#selected_photo_link").html(
            getJetPhotosPhotoLink(selected.registration)
        );
    } else {
        $("#selected_photo_link").html(
            getFlightAwarePhotoLink(selected.registration)
        );
    }
}

// Refreshes the larger table of all the planes
function refreshTableInfo() {
    checkSidebarWidthChange(); // AKISSACK mapsize    Ref: AKDD

    var show_squawk_warning = false;

    TrackedAircraft = 0;
    TrackedAircraftPositions = 0;
    TrackedHistorySize = 0;
    CurMaxRange = 0; // AKISSACK  Ref: AK9T
    CurMinRange = 999999; // AKISSACK  Ref: AK9T

    $(".altitudeUnit").text(get_unit_label("altitude", DisplayUnits));
    $(".speedUnit").text(get_unit_label("speed", DisplayUnits));
    $(".distanceUnit").text(get_unit_label("distance", DisplayUnits));
    $(".verticalRateUnit").text(get_unit_label("verticalRate", DisplayUnits));

    for (var i = 0; i < PlanesOrdered.length; ++i) {
        var tableplane = PlanesOrdered[i];
        TrackedHistorySize += tableplane.history_size;
        if (tableplane.seen >= 58 || tableplane.isFiltered()) {
            tableplane.tr.className = "plane_table_row hidden";
        } else {
            TrackedAircraft++;
            // AKISSACK Range display  Ref: AK9T
            if (CurMaxRange < tableplane.sitedist) {
                // AKISSACK
                CurMaxRange = tableplane.sitedist;
                if (CurMaxRange > MaxRange) {
                    MaxRange = CurMaxRange;
                }
                //console.log("+"+CurMaxRange);
            }
            if (tableplane.sitedist && CurMinRange > tableplane.sitedist) {
                // AKISSACK
                CurMinRange = tableplane.sitedist;
                //console.log("-"+CurMinRange);
            }

            var classes = "plane_table_row";

            if (tableplane.position !== null) {
                if (tableplane.seen_pos < 60) {
                    ++TrackedAircraftPositions;
                    if (tableplane.position_from_mlat) {
                        classes += " mlat"; 
                    } else classes += " vPosition";
                } else classes += " acdefault";
            } else {
                 classes += " acdefaultnopos";
	        }
            if (tableplane.icao == SelectedPlane) classes += " selected";

            if (tableplane.is_interesting == "Y") { // AKISSACK ------------ Ref: AK9F
                classes += " ofInterest ";
            }

            if (tableplane.squawk in SpecialSquawks) {
                classes = classes + " " + SpecialSquawks[tableplane.squawk].cssClass;
                show_squawk_warning = true;
            }

            if (ShowMyPreferences) {
                tableplane.tr.cells[0].innerHTML = getAirframesModeSLinkIcao(
                    tableplane.icao
                ); // AKISSACK ------------ Ref: AK9F
                tableplane.tr.cells[2].textContent =
                    tableplane.flight !== null ? tableplane.flight : "";
            } else {
                // ICAO doesn't change
                if (tableplane.flight) {
                    tableplane.tr.cells[2].innerHTML = getFlightAwareModeSLink(
                        tableplane.icao,
                        tableplane.flight,
                        tableplane.flight
                    );
                } else {
                    tableplane.tr.cells[2].innerHTML = "";
                }
            }
            if (ShowMyPreferences && ShowHTMLColumns) {
                // ------------ Ref: AK9F
                tableplane.tr.cells[3].textContent =
                    tableplane.registration !== null ? tableplane.registration : "";
                tableplane.tr.cells[4].textContent =
                    tableplane.icaotype !== null ? tableplane.icaotype : "";
                var tmpTxt1 =
                    tableplane.ac_aircraft !== null ? tableplane.ac_aircraft : "-";
                if (tmpTxt1 === "-" || tmpTxt1 === "") {
                    //  Let's try an alternative to ID -> https://github.com/alkissack/Dump1090-OpenLayers-html/issues/3
                    tmpTxt1 = tableplane.icaotype
                        ? tableplane.icaotype
                        : "Unknown aircraft";
                    //console.log("-"+tmpTxt1 );
                }
                //tableplane.tr.cells[5].textContent = (tableplane.ac_aircraft !== null ? tableplane.ac_aircraft : "");
                tableplane.tr.cells[5].textContent = tmpTxt1;

                tmpTxt1 =
                    tableplane.ac_shortname !== null ? tableplane.ac_shortname : "-";
                if (tmpTxt1 === "-" || tmpTxt1 === "") {
                    //  Let's try an alternative to ID -> https://github.com/alkissack/Dump1090-OpenLayers3-html/issues/3
                    tmpTxt1 = tableplane.icaotype ? tableplane.icaotype : "Unknown";
                    //console.log("-"+tmpTxt1 );
                }
                //tableplane.tr.cells[6].textContent = (tableplane.ac_shortname !== null ? tableplane.ac_shortname : "");
                tableplane.tr.cells[6].textContent = tmpTxt1;

                tableplane.tr.cells[7].textContent =
                    tableplane.ac_category !== null ? tableplane.ac_category : "";
                tableplane.tr.cells[8].textContent =
                    tableplane.squawk !== null ? tableplane.squawk : "";
                tableplane.tr.cells[9].innerHTML = format_altitude_brief(
                    tableplane.altitude,
                    tableplane.vert_rate,
                    DisplayUnits
                );
                tableplane.tr.cells[10].textContent = format_speed_brief(
                    tableplane.speed,
                    DisplayUnits
                );
                tableplane.tr.cells[11].textContent = format_vert_rate_brief(
                    tableplane.vert_rate,
                    DisplayUnits
                );
                tableplane.tr.cells[12].textContent = format_distance_brief(
                    tableplane.sitedist,
                    DisplayUnits
                ); // Column index change needs to be reflected above in initialize_map()
                tableplane.tr.cells[13].textContent = format_track_brief(
                    tableplane.track
                );
                tableplane.tr.cells[14].textContent = tableplane.messages;
                tableplane.tr.cells[15].textContent = tableplane.seen.toFixed(0);
                tableplane.tr.cells[16].textContent =
                    tableplane.rssi !== null ? tableplane.rssi : "";
                tableplane.tr.cells[17].textContent =
                    tableplane.position !== null ? tableplane.position[1].toFixed(4) : "";
                tableplane.tr.cells[18].textContent =
                    tableplane.position !== null ? tableplane.position[0].toFixed(4) : "";
                tableplane.tr.cells[19].textContent = format_data_source(
                    tableplane.getDataSource()
                );
                tableplane.tr.cells[20].innerHTML = getAirframesModeSLink(
                    tableplane.icao
                );
                tableplane.tr.cells[21].innerHTML = getFlightAwareModeSLink(
                    tableplane.icao,
                    tableplane.flight
                );
                if (UseJetPhotosPhotoLink) {
                    tableplane.tr.cells[22].innerHTML = getJetPhotosPhotoLink(
                        tableplane.registration
                    );
                } else {
                    tableplane.tr.cells[22].innerHTML = getFlightAwarePhotoLink(
                        tableplane.registration
                    );
                }
                tableplane.tr.className = classes;
            } else {
                tableplane.tr.cells[3].textContent =
                    tableplane.registration !== null ? tableplane.registration : "";
                tableplane.tr.cells[4].textContent =
                    tableplane.icaotype !== null ? tableplane.icaotype : "";
                tableplane.tr.cells[5].textContent =
                    tableplane.squawk !== null ? tableplane.squawk : "";
                tableplane.tr.cells[6].innerHTML = format_altitude_brief(
                    tableplane.altitude,
                    tableplane.vert_rate,
                    DisplayUnits
                );
                tableplane.tr.cells[7].textContent = format_speed_brief(
                    tableplane.speed,
                    DisplayUnits
                );
                tableplane.tr.cells[8].textContent = format_vert_rate_brief(
                    tableplane.vert_rate,
                    DisplayUnits
                );
                tableplane.tr.cells[9].textContent = format_distance_brief(
                    tableplane.sitedist,
                    DisplayUnits
                );
                tableplane.tr.cells[10].textContent = format_track_brief(
                    tableplane.track
                );
                tableplane.tr.cells[11].textContent = tableplane.messages;
                tableplane.tr.cells[12].textContent = tableplane.seen.toFixed(0);
                tableplane.tr.cells[13].textContent =
                    tableplane.rssi !== null ? tableplane.rssi : "";
                tableplane.tr.cells[14].textContent =
                    tableplane.position !== null ? tableplane.position[1].toFixed(4) : "";
                tableplane.tr.cells[15].textContent =
                    tableplane.position !== null ? tableplane.position[0].toFixed(4) : "";
                tableplane.tr.cells[16].textContent = format_data_source(
                    tableplane.getDataSource()
                );
                tableplane.tr.cells[17].innerHTML = getAirframesModeSLink(
                    tableplane.icao
                );
                tableplane.tr.cells[18].innerHTML = getFlightAwareModeSLink(
                    tableplane.icao,
                    tableplane.flight
                );
                if (UseJetPhotosPhotoLink) {
                    tableplane.tr.cells[19].innerHTML = getJetPhotosPhotoLink(
                        tableplane.registration
                    );
                } else {
                    tableplane.tr.cells[19].innerHTML = getFlightAwarePhotoLink(
                        tableplane.registration
                    );
                }
                tableplane.tr.className = classes;
            }
        }
    }

    if (show_squawk_warning) {
        $("#SpecialSquawkWarning").css("display", "block");
    } else {
        $("#SpecialSquawkWarning").css("display", "none");
    }

    resortTable();

    // AKISSACK - Range Plots Ref: AK8E
    MaxRangeFeatures.clear();
    MidRangeFeatures.clear();
    MinRangeFeatures.clear();

    // MAXIMUM ------------------------------------
    var style = new ol.style.Style({
        stroke: new ol.style.Stroke({
            color: "rgba(0,0,128, 1)",
            width: RangeLine,
        }),
        fill: new ol.style.Fill({
            color: "rgba(0,0,255, 0.05)",
        }),
    });

    var polyCoords = [];
    for (var i = 0; i < 720; i++) {
        polyCoords.push(
            ol.proj.transform([MaxRngLon[i], MaxRngLat[i]], "EPSG:4326", "EPSG:3857")
        );
    }
    var rangeFeature = new ol.Feature({
        geometry: new ol.geom.Polygon([polyCoords]),
    });
    rangeFeature.setStyle(style);
    if (MaxRangeShow) {
        MaxRangeFeatures.push(rangeFeature);
    }

    // MEDIUM ------------------------------------
    var style = new ol.style.Style({
        stroke: new ol.style.Stroke({
            color: "rgba(0,128,0, 0.5)",
            width: RangeLine,
        }),
        fill: new ol.style.Fill({
            color: "rgba(0,255,0, 0.05)",
        }),
    });
    var polyCoords = [];
    for (var i = 0; i < 720; i++) {
        polyCoords.push(
            ol.proj.transform([MidRngLon[i], MidRngLat[i]], "EPSG:4326", "EPSG:3857")
        );
    }
    var rangeFeature = new ol.Feature({
        geometry: new ol.geom.Polygon([polyCoords]),
    });
    rangeFeature.setStyle(style);
    if (MidRangeShow && MidRangeHeight > 0) {
        MidRangeFeatures.push(rangeFeature);
    } // Medium range

    // MINIMUM ------------------------------------
    var style = new ol.style.Style({
        stroke: new ol.style.Stroke({
            color: "rgba(128,0,0, 0.5)",
            width: RangeLine,
        }),
        fill: new ol.style.Fill({
            color: "rgba(255,0,0, 0.05)",
        }),
    });
    var polyCoords = [];
    for (var i = 0; i < 720; i++) {
        polyCoords.push(
            ol.proj.transform([MinRngLon[i], MinRngLat[i]], "EPSG:4326", "EPSG:3857")
        );
    }
    var rangeFeature = new ol.Feature({
        geometry: new ol.geom.Polygon([polyCoords]),
    });
    rangeFeature.setStyle(style);
    //if (MinRangeHeight > 0) {
    if (MinRangeShow && MinRangeHeight > 0) {
        MinRangeFeatures.push(rangeFeature);
    } // Minimum range
}

//
// ---- table sorting ----
//

function compareAlpha(xa, ya) {
    if (xa === ya) return 0;
    if (xa < ya) return -1;
    return 1;
}

function compareNumeric(xf, yf) {
    if (Math.abs(xf - yf) < 1e-9) return 0;

    return xf - yf;
}

function sortByICAO() {
    sortBy("icao", compareAlpha, function (x) {
        return x.icao;
    });
}
function sortByFlight() {
    sortBy("flight", compareAlpha, function (x) {
        return x.flight;
    });
}
function sortByRegistration() {
    sortBy("registration", compareAlpha, function (x) {
        return x.registration;
    });
}
function sortByAircraftType() {
    sortBy("icaotype", compareAlpha, function (x) {
        return x.icaotype;
    });
}
function sortBySquawk() {
    sortBy("squawk", compareAlpha, function (x) {
        return x.squawk;
    });
}
function sortByAltitude() {
    sortBy("altitude", compareNumeric, function (x) {
        return x.altitude == "ground" ? -1e9 : x.altitude;
    });
}
function sortBySpeed() {
    sortBy("speed", compareNumeric, function (x) {
        return x.speed;
    });
}
function sortByVerticalRate() {
    sortBy("vert_rate", compareNumeric, function (x) {
        return x.vert_rate;
    });
}
function sortByDistance() {
    // AKISSACK - Order by distance, but show interesting aircraft first in the table  ------------ Ref: AK9F
    if (ShowMyPreferences) {
        sortBy("sitedist", compareNumeric, function (x) {
            return x.is_interesting == "Y"
                ? x.sitedist + 0
                : x.sitedist == null
                    ? null
                    : x.sitedist + 1000000;
        });
    } else {
        sortBy("sitedist", compareNumeric, function (x) {
            return x.sitedist;
        });
    }
}
function sortByTrack() {
    sortBy("track", compareNumeric, function (x) {
        return x.track;
    });
}
function sortByMsgs() {
    sortBy("msgs", compareNumeric, function (x) {
        return x.messages;
    });
}
function sortBySeen() {
    sortBy("seen", compareNumeric, function (x) {
        return x.seen;
    });
}
function sortByCountry() {
    sortBy("country", compareAlpha, function (x) {
        return x.icaorange.country;
    });
}
function sortByRssi() {
    sortBy("rssi", compareNumeric, function (x) {
        return x.rssi;
    });
}
function sortByLatitude() {
    sortBy("lat", compareNumeric, function (x) {
        return x.position !== null ? x.position[1] : null;
    });
}
function sortByLongitude() {
    sortBy("lon", compareNumeric, function (x) {
        return x.position !== null ? x.position[0] : null;
    });
}
function sortByDataSource() {
    sortBy("data_source", compareAlpha, function (x) {
        return x.getDataSource();
    });
}

var sortId = "";
var sortCompare = null;
var sortExtract = null;
var sortAscending = true;

function sortFunction(x, y) {
    var xv = x._sort_value;
    var yv = y._sort_value;

    // always sort missing values at the end, regardless of
    // ascending/descending sort
    if (xv == null && yv == null) return x._sort_pos - y._sort_pos;
    if (xv == null) return 1;
    if (yv == null) return -1;

    var c = sortAscending ? sortCompare(xv, yv) : sortCompare(yv, xv);
    if (c !== 0) return c;

    return x._sort_pos - y._sort_pos;
}

function resortTable() {
    // number the existing rows so we can do a stable sort
    // regardless of whether sort() is stable or not.
    // Also extract the sort comparison value.
    for (var i = 0; i < PlanesOrdered.length; ++i) {
        PlanesOrdered[i]._sort_pos = i;
        PlanesOrdered[i]._sort_value = sortExtract(PlanesOrdered[i]);
    }

    PlanesOrdered.sort(sortFunction);

    var tbody = document.getElementById("tableinfo").tBodies[0];
    for (var i = 0; i < PlanesOrdered.length; ++i) {
        tbody.appendChild(PlanesOrdered[i].tr);
    }
}

function sortBy(id, sc, se) {
    if (id === sortId) {
        sortAscending = !sortAscending;
        PlanesOrdered.reverse(); // this correctly flips the order of rows that compare equal
    } else {
        sortAscending = true;
    }

    sortId = id;
    sortCompare = sc;
    sortExtract = se;

    resortTable();
}

function selectPlaneByHex(hex, autofollow) {
    //console.log("select: " + hex);
    // If SelectedPlane has something in it, clear out the selected

    if (SelectedAllPlanes) {
        deselectAllPlanes();
    }

    // -------------------------------------------------------------------
    // AKISSACK - Allow multiple selections                         [MLTI]
    // -------------------------------------------------------------------
    //if (SelectedPlane != null) {
    //	Planes[SelectedPlane].selected = false;
    //	Planes[SelectedPlane].clearLines();
    //	Planes[SelectedPlane].updateMarker();
    //        $(Planes[SelectedPlane].tr).removeClass("selected");
    //}
    // -------------------------------------------------------------------
    // ------------------------------------------------------- AKISSACK
    // -------------------------------------------------------------------

    // If we are clicking the same plane, we are deselecting it.
    // (unless it was a doubleclick..)
    if (SelectedPlane === hex && !autofollow) {
        hex = null;
    }

    if (hex !== null) {
        // Assign the new selected
        SelectedPlane = hex;
        Planes[SelectedPlane].selected = true;
        Planes[SelectedPlane].updateLines();
        Planes[SelectedPlane].updateMarker();
        $(Planes[SelectedPlane].tr).addClass("selected");
    } else {
        SelectedPlane = null;
    }

    if (SelectedPlane !== null && autofollow) {
        FollowSelected = true;
        if (OLMap.getView().getZoom() < 8) OLMap.getView().setZoom(8);
    } else {
        FollowSelected = false;
    }

    refreshSelected();
}

// loop through the planes and mark them as selected to show the paths for all planes
function selectAllPlanes() {
    // if all planes are already selected, deselect them all
    if (SelectedAllPlanes) {
        deselectAllPlanes();
    } else {
        // If SelectedPlane has something in it, clear out the selected
        if (SelectedPlane != null) {
            Planes[SelectedPlane].selected = false;
            Planes[SelectedPlane].clearLines();
            Planes[SelectedPlane].updateMarker();
            $(Planes[SelectedPlane].tr).removeClass("selected");
        }

        SelectedPlane = null;
        SelectedAllPlanes = true;

        for (var key in Planes) {
            if (Planes[key].visible && !Planes[key].isFiltered()) {
                Planes[key].selected = true;
                Planes[key].updateLines();
                Planes[key].updateMarker();
            }
        }
    }

    refreshSelected();
}

// AKISSACK --------------- Ref: AK9G
function selectMilPlanes() {
    // if mil planes are already selected, deselect them all
    //console.log("mil "+SelectedMilPlanes);
    if (SelectedMilPlanes) {
        deselectMilPlanes();
    } else {
        // If SelectedPlane has something in it, clear out the selected
        if (SelectedPlane != null) {
            Planes[SelectedPlane].selected = false;
            Planes[SelectedPlane].clearLines();
            Planes[SelectedPlane].updateMarker();
            $(Planes[SelectedPlane].tr).removeClass("selected");
        }

        SelectedPlane = null;
        SelectedMilPlanes = true;

        for (var key in Planes) {
            if (
                Planes[key].visible &&
                !Planes[key].isFiltered() &&
                Planes[key].my_trail
            ) {
                Planes[key].selected = true;
                Planes[key].updateLines();
                Planes[key].updateMarker();
            }
        }
    }

    refreshSelected();
}

// deselect all the mil' planes
function deselectMilPlanes() {
    for (var key in Planes) {
        Planes[key].selected = false;
        Planes[key].clearLines();
        Planes[key].updateMarker();
        $(Planes[key].tr).removeClass("selected");
    }
    SelectedPlane = null;
    SelectedMilPlanes = false;
    refreshSelected();
}
// ----------------- AKISSACK

// on refreshes, try to find new planes and mark them as selected
function selectNewPlanes() {
    if (SelectedAllPlanes) {
        for (var key in Planes) {
            if (!Planes[key].visible || Planes[key].isFiltered()) {
                Planes[key].selected = false;
                Planes[key].clearLines();
                Planes[key].updateMarker();
            } else {
                if (Planes[key].selected !== true) {
                    Planes[key].selected = true;
                    Planes[key].updateLines();
                    Planes[key].updateMarker();
                }
            }
        }
    }
}

// deselect all the planes
function deselectAllPlanes() {
    for (var key in Planes) {
        Planes[key].selected = false;
        Planes[key].clearLines();
        Planes[key].updateMarker();
        $(Planes[key].tr).removeClass("selected");
    }
    SelectedPlane = null;
    SelectedAllPlanes = false;
    AllwaysShowPermanentLabels = false;
    deselectMilPlanes();
    refreshSelected();
}

function toggleFollowSelected() {
    FollowSelected = !FollowSelected;
    if (FollowSelected && OLMap.getView().getZoom() < 8)
        OLMap.getView().setZoom(8);
    refreshSelected();
}

function resetMap() {
    // Reset localStorage values and map settings
    localStorage["CenterLat"] = CenterLat = DefaultCenterLat;
    localStorage["CenterLon"] = CenterLon = DefaultCenterLon;
    localStorage["ZoomLvl"] = ZoomLvl = DefaultZoomLvl;

    // Set and refresh
    OLMap.getView().setZoom(ZoomLvl);
    OLMap.getView().setCenter(ol.proj.fromLonLat([CenterLon, CenterLat]));

    selectPlaneByHex(null, false);
}

function resetRangePlot() {
    for (var j = 0; j < 720; j++) {  // 360 --> 720 ref: Github issue #17
        MaxRngRange[j] = 0;
        MaxRngLat[j] = SiteLat;
        MaxRngLon[j] = SiteLon;
        MidRngRange[j] = MaxRngRange[j];
        MidRngLat[j] = MaxRngLat[j];
        MidRngLon[j] = MaxRngLon[j];
        MinRngRange[j] = MaxRngRange[j];
        MinRngLat[j] = MaxRngLat[j];
        MinRngLon[j] = MaxRngLon[j];
    }
}

function exportRangePlot() {
    var rangemax = [];
    var rangemid = [];
    var rangemin = [];

    for (var j = 0; j < 720; j++) {  // 360 --> 720 ref: Github issue #17
        rangemax[j] = [j, MaxRngRange[j], MaxRngLat[j], MaxRngLon[j]]
        rangemid[j] = [j, MidRngRange[j], MidRngLat[j], MidRngLon[j]]
        rangemin[j] = [j, MinRngRange[j], MinRngLat[j], MinRngLon[j]]
    }

    const datamax = JSON.stringify(rangemax);
    const datamid = JSON.stringify(rangemid);
    const datamin = JSON.stringify(rangemin);

    //console.log("data.json written correctly " + datamax);

    const link = document.createElement("a");

    var blob = new Blob([datamax], {type: "text/plain;charset=utf-8",}); // Create blob object with file content
    link.href = URL.createObjectURL(blob);   // Add file content in the object URL
    link.download = "maxRange.json";         // Add file name
    link.click();                            // Add click event to <a> tag to save file.
    URL.revokeObjectURL(link.href);

    setTimeout(function(){                   // ref: Github issue #19
        blob = new Blob([datamid], {type: "text/plain;charset=utf-8",});
        link.href = URL.createObjectURL(blob);
        link.download = "midRange.json";
        link.click();
        URL.revokeObjectURL(link.href);
        setTimeout(function(){
            blob = new Blob([datamin], {type: "text/plain;charset=utf-8",});
            link.href = URL.createObjectURL(blob);
            link.download = "minRange.json";
            link.click();
            URL.revokeObjectURL(link.href);
        },3000); //delay is in milliseconds 

    },3000); //delay is in milliseconds 

}

function importRangePlot() {
    fetch('./backup/maxRange.json')
        .then((response) => response.json())
        .then((json) => importMax(json));

    fetch('./backup/midRange.json')
        .then((response) => response.json())
        .then((json) => importMid(json));

    fetch('./backup/minRange.json')
        .then((response) => response.json())
        .then((json) => importMin(json));

}

function importMax(json) {
    //console.log(json.length);
    if (json.length === 360) {
      for (var j = 0; j < json.length; j++) {
          var obj = json[j];
          var rslot = 2 * obj[0];
          var rslotnew = rslot +1 ; 
          MaxRngRange[rslot] = obj[1];
          MaxRngLat[rslot]   = obj[2];
          MaxRngLon[rslot]   = obj[3]

          MaxRngRange[rslotnew] = obj[1];
          MaxRngLat[rslotnew]   = obj[2];
          MaxRngLon[rslotnew]   = obj[3]
      }
    } else { // 360 --> 720 ref: Github issue #17
      for (var j = 0; j < json.length; j++) {
          var obj = json[j];
          MaxRngRange[obj[0]] = obj[1];
          MaxRngLat[obj[0]]   = obj[2];
          MaxRngLon[obj[0]]   = obj[3]
      }
    } 
    //console.log(MaxRngRange +" "+MaxRngLat+" "+MaxRngLon);
}

function importMid(json) {
    if (json.length === 360) {
      for (var j = 0; j < json.length; j++) {
          var obj = json[j];
          var rslot = 2 * obj[0];
          var rslotnew = rslot +1 ; 
          MidRngRange[rslot] = obj[1];
          MidRngLat[rslot]   = obj[2];
          MidRngLon[rslot]   = obj[3]

          MidRngRange[rslotnew] = obj[1];
          MidRngLat[rslotnew]   = obj[2];
          MidRngLon[rslotnew]   = obj[3]
      }
    } else { // 360 --> 720 ref: Github issue #17
      for (var j = 0; j < json.length; j++) {
          var obj = json[j];
          MidRngRange[obj[0]] = obj[1];
          MidRngLat[obj[0]]   = obj[2];
          MidRngLon[obj[0]]   = obj[3]
      }
    } 
}

function importMin(json) {
    if (json.length === 360) {
      for (var j = 0; j < json.length; j++) {
          var obj = json[j];
          var rslot = 2 * obj[0];
          var rslotnew = rslot +1 ; 
          MinRngRange[rslot] = obj[1];
          MinRngLat[rslot]   = obj[2];
          MinRngLon[rslot]   = obj[3]

          MinRngRange[rslotnew] = obj[1];
          MinRngLat[rslotnew]   = obj[2];
          MinRngLon[rslotnew]   = obj[3]
      }
    } else { // 360 --> 720 ref: Github issue #17
      for (var j = 0; j < json.length; j++) {
          var obj = json[j];
          MinRngRange[obj[0]] = obj[1];
          MinRngLat[obj[0]]   = obj[2];
          MinRngLon[obj[0]]   = obj[3]
      }
    } 
}


//function importMin(json) {
//    for (var j = 0; j < json.length; j++) {
//        var obj = json[j];
//        MinRngRange[obj[0]] = obj[1];
//        MinRngLat[obj[0]]   = obj[2];
//        MinRngLon[obj[0]]   = obj[3]
//    }
//}

function updateMapSize() {
    OLMap.updateSize();
}

function toggleSidebarVisibility(e) {
    e.preventDefault();
    $("#sidebar_container").toggle();
    $("#expand_sidebar_control").toggle();
    $("#toggle_sidebar_button").toggleClass("show_sidebar");
    $("#toggle_sidebar_button").toggleClass("hide_sidebar");
    updateMapSize();
}

function expandSidebar(e) {
    e.preventDefault();
    $("#map_container").hide();
    $("#toggle_sidebar_control").hide();
    $("#splitter").hide();
    $("#sudo_buttons").hide();
    $("#show_range_admin_buttons").show();
    $("#show_map_button").show();
    $("#sidebar_container").width("100%");
    setColumnVisibility();
    setSelectedInfoBlockVisibility();
    updateMapSize();
}

function showMap() {
    $("#map_container").show();
    $("#toggle_sidebar_control").show();
    $("#splitter").show();
    $("#show_range_admin_buttons").hide();
    $("#sudo_buttons").show();
    $("#show_map_button").hide();
    $("#sidebar_container").width("auto");
    setColumnVisibility();
    setSelectedInfoBlockVisibility();
    updateMapSize();
}

function showColumn(table, columnId, visible) {
    var index = $(columnId).index();
    if (index >= 0) {
        var cells = $(table).find("td:nth-child(" + (index + 1).toString() + ")");
        if (visible) {
            cells.show();
        } else {
            cells.hide();
        }
    }
}

function setColumnVisibility() {
    var mapIsVisible = $("#map_container").is(":visible");
    var infoTable = $("#tableinfo");

    showColumn(infoTable, "#registration", !mapIsVisible);
    if (ShowMyPreferences) {
        // AKISSACK - Adjust table columns ------------------ Ref: AK9F
        showColumn(infoTable, "#aircraft_type", !mapIsVisible);
        showColumn(infoTable, "#myAc", !mapIsVisible);
        showColumn(infoTable, "#myAcCat", !mapIsVisible);
        showColumn(infoTable, "#myAcType", !mapIsVisible);
        showColumn(infoTable, "#msgs", !mapIsVisible);
        showColumn(infoTable, "#seen", !mapIsVisible);
        showColumn(infoTable, "#vert_rate", !mapIsVisible);
        showColumn(infoTable, "#rssi", !mapIsVisible);
        showColumn(infoTable, "#lat", !mapIsVisible);
        showColumn(infoTable, "#lon", !mapIsVisible);
        showColumn(infoTable, "#data_source", !mapIsVisible);
        showColumn(infoTable, "#airframes_mode_s_link", !mapIsVisible);
        showColumn(infoTable, "#flightaware_mode_s_link", !mapIsVisible);
        showColumn(infoTable, "#flightaware_photo_link", !mapIsVisible);
    } else {
        showColumn(infoTable, "#aircraft_type", !mapIsVisible);
        showColumn(infoTable, "#vert_rate", !mapIsVisible);
        showColumn(infoTable, "#rssi", !mapIsVisible);
        showColumn(infoTable, "#lat", !mapIsVisible);
        showColumn(infoTable, "#lon", !mapIsVisible);
        showColumn(infoTable, "#data_source", !mapIsVisible);
        showColumn(infoTable, "#airframes_mode_s_link", !mapIsVisible);
        showColumn(infoTable, "#flightaware_mode_s_link", !mapIsVisible);
        showColumn(infoTable, "#flightaware_photo_link", !mapIsVisible);
    }
}

function setSelectedInfoBlockVisibility() {
    var mapIsVisible = $("#map_container").is(":visible");
    var planeSelected =
        typeof SelectedPlane !== "undefined" &&
        SelectedPlane != null &&
        SelectedPlane != "ICAO";

    if (planeSelected && mapIsVisible) {
        $("#selected_infoblock").show();
    } else {
        $("#selected_infoblock").hide();
    }
}

// Reposition selected plane info box if it overlaps plane marker
function adjustSelectedInfoBlockPosition() {
    if (
        typeof Planes === "undefined" ||
        typeof SelectedPlane === "undefined" ||
        Planes === null
    ) {
        return;
    }

    var selectedPlane = Planes[SelectedPlane];
    if (
        selectedPlane === undefined ||
        selectedPlane === null ||
        selectedPlane.marker === undefined ||
        selectedPlane.marker === null
    ) {
        return;
    }

    try {
        // Get marker position
        var marker = selectedPlane.marker;
        var markerCoordinates = selectedPlane.marker.getGeometry().getCoordinates();
        var markerPosition = OLMap.getPixelFromCoordinate(markerCoordinates);

        // Get info box position and size
        var infoBox = $("#selected_infoblock");
        var infoBoxPosition = infoBox.position();
        var infoBoxExtent = getExtent(
            infoBoxPosition.left,
            infoBoxPosition.top,
            infoBox.outerWidth(),
            infoBox.outerHeight()
        );

        // Get map size
        var mapCanvas = $("#map_canvas");
        var mapExtent = getExtent(0, 0, mapCanvas.width(), mapCanvas.height());

        // Check for overlap
        if (
            isPointInsideExtent(markerPosition[0], markerPosition[1], infoBoxExtent)
        ) {
            // Array of possible new positions for info box
            var candidatePositions = [];
            candidatePositions.push({ x: 20, y: 20 });
            candidatePositions.push({ x: 20, y: markerPosition[1] + 40 });

            // Find new position
            for (var i = 0; i < candidatePositions.length; i++) {
                var candidatePosition = candidatePositions[i];
                var candidateExtent = getExtent(
                    candidatePosition.x,
                    candidatePosition.y,
                    infoBox.outerWidth(),
                    infoBox.outerHeight()
                );

                if (
                    !isPointInsideExtent(
                        markerPosition[0],
                        markerPosition[1],
                        candidateExtent
                    ) &&
                    isPointInsideExtent(
                        candidatePosition.x,
                        candidatePosition.y,
                        mapExtent
                    )
                ) {
                    // Found a new position that doesn't overlap marker - move box to that position
                    infoBox.css("left", candidatePosition.x);
                    infoBox.css("top", candidatePosition.y);
                    return;
                }
            }
        }
    } catch (e) { }
}

function getExtent(x, y, width, height) {
    return {
        xMin: x,
        yMin: y,
        xMax: x + width - 1,
        yMax: y + height - 1,
    };
}

function isPointInsideExtent(x, y, extent) {
    return (
        x >= extent.xMin && x <= extent.xMax && y >= extent.yMin && y <= extent.yMax
    );
}

function initializeUnitsSelector() {
    // Get display unit preferences from local storage
    if (
        !localStorage.getItem("displayUnits") ||
        localStorage.getItem("displayUnits") != DisplayUnits
    ) {
        localStorage["displayUnits"] = DisplayUnits;
    }
    var displayUnits = localStorage["displayUnits"];
    DisplayUnits = displayUnits;

    // Initialize drop-down
    var unitsSelector = $("#units_selector");
    unitsSelector.val(displayUnits);
    unitsSelector.on("change", onDisplayUnitsChanged);
}

function onDisplayUnitsChanged(e) {
    var displayUnits = event.target.value;
    // Save display units to local storage
    localStorage["displayUnits"] = displayUnits;
    DisplayUnits = displayUnits;

    // Update filters
    updatePlaneFilter();

    // Refresh data
    refreshTableInfo();
    refreshSelected();

    // Redraw range rings
    if (SitePosition !== null && SitePosition !== undefined && SiteCircles) {
        createSiteCircleFeatures();
    }

    // Reset map scale line units
    OLMap.getControls().forEach(function (control) {
        if (control instanceof ol.control.ScaleLine) {
            control.setUnits(displayUnits);
        }
    });
}

function onFilterByAltitude(e) {
    e.preventDefault();
    updatePlaneFilter();
    //console.log( PlaneFilter.specials);   //AKISSACK
    refreshTableInfo();

    var selectedPlane = Planes[SelectedPlane];
    if (
        selectedPlane !== undefined &&
        selectedPlane !== null &&
        selectedPlane.isFiltered()
    ) {
        SelectedPlane = null;
        selectedPlane.selected = false;
        selectedPlane.clearLines();
        selectedPlane.updateMarker();
        refreshSelected();
    }
}

function onResetAltitudeFilter(e) {
    $("#altitude_filter_min").val("");
    $("#altitude_filter_max").val("");
    // ------------------------------------------------------------------
    // Allow filtering by special aircraft       AKISSACK Ref: AK11C -->
    // ------------------------------------------------------------------
    $("#specials_filter").prop("checked", false); //AKISSACK      // <--- ENDS

    updatePlaneFilter();
    refreshTableInfo();
}

function updatePlaneFilter() {
    var minAltitude = parseFloat($("#altitude_filter_min").val().trim());
    var maxAltitude = parseFloat($("#altitude_filter_max").val().trim());
    var specialsOnly = $("#specials_filter").is(":checked"); // Allow filtering by special aircraft       AKISSACK Ref: AK11D
    // console.log(specialsOnly );

    if (minAltitude === NaN) {
        minAltitude = -Infinity;
    }

    if (maxAltitude === NaN) {
        maxAltitude = Infinity;
    }

    PlaneFilter.specials = specialsOnly; // Allow filtering by special aircraft       AKISSACK Ref: AK11D
    PlaneFilter.minAltitude = minAltitude;
    PlaneFilter.maxAltitude = maxAltitude;
    PlaneFilter.altitudeUnits = DisplayUnits;
}

function getFlightAwareIdentLink(ident, linkText) {
    if (ident !== null && ident !== "") {
        if (!linkText) {
            linkText = ident;
        }
        return (
            '<a target="_blank" href="https://flightaware.com/live/flight/' +
            ident.trim() + '">' + linkText + "</a>"
        );
    }
    return "";
}

function getFlightAwareModeSLink(code, ident, linkText) {
    if (
        code !== null &&
        code.length > 0 &&
        code[0] !== "~" &&
        code !== "000000"
    ) {
        if (!linkText) {
            linkText = "FlightAware: " + code.toUpperCase();
        }

        var linkHtml =
            '<a target="_blank" href="https://flightaware.com/live/modes/' + code;
        if (ident !== null && ident !== "") {
            linkHtml += "/ident/" + ident.trim();
        }
        linkHtml += '/redirect">' + linkText + "</a>";
        return linkHtml;
    }
    return "";
}

function getFlightAwarePhotoLink(registration) {
    if (registration !== null && registration !== "") {
        return (
            '<a target="_blank" href="https://flightaware.com/photos/aircraft/' +
            registration.trim() +
            '">See Photos</a>'
        );
    }
    return "";
}

function getJetPhotosPhotoLink(registration) {
    if (registration !== null && registration !== "") {
        return (
            '<a target="_blank" href="https://www.jetphotos.com/registration/' +
            registration.trim() +
            '">See Photos</a>'
        );
    }
    return "";
}

function getAirframesModeSLink(code) {
    if (
        code !== null &&
        code.length > 0 &&
        code[0] !== "~" &&
        code !== "000000"
    ) {
        return (
            "<a href=\"http://www.airframes.org/\" onclick=\"$('#airframes_post_icao').attr('value','" +
            code +
            "'); document.getElementById('horrible_hack').submit.call(document.getElementById('airframes_post')); return false;\">Airframes.org: " +
            code.toUpperCase() +
            "</a>"
        );
    }
    return "";
}

function getAirframesModeSLinkIcao(code) {
    // AKISSACK  Ref: AK9F
    if (
        code !== null &&
        code.length > 0 &&
        code[0] !== "~" &&
        code !== "000000"
    ) {
        return (
            "<a href=\"http://www.airframes.org/\" onclick=\"$('#airframes_post_icao').attr('value','" +
            code +
            "'); document.getElementById('horrible_hack').submit.call(document.getElementById('airframes_post')); return false;\">" +
            code.toUpperCase() +
            "</a>"
        );
    }
    return "";
}

function getTerrainColorByAlti(alti) {
    var s = TerrainColorByAlt.s;
    var l = TerrainColorByAlt.l;

    // find the pair of points the current altitude lies between,
    // and interpolate the hue between those points
    var hpoints = TerrainColorByAlt.h;
    var h = hpoints[0].val;
    for (var i = hpoints.length - 1; i >= 0; --i) {
        if (alti > hpoints[i].alt) {
            if (i == hpoints.length - 1) {
                h = hpoints[i].val;
            } else {
                h =
                    hpoints[i].val +
                    ((hpoints[i + 1].val - hpoints[i].val) * (alti - hpoints[i].alt)) /
                    (hpoints[i + 1].alt - hpoints[i].alt);
            }
            break;
        }
    }

    if (h < 0) {
        h = (h % 360) + 360;
    } else if (h >= 360) {
        h = h % 360;
    }

    if (s < 5) s = 5;
    else if (s > 95) s = 95;

    if (l < 5) l = 5;
    else if (l > 95) l = 95;

    return (
        "hsl(" +
        (h / 5).toFixed(0) * 5 +
        "," +
        (s / 5).toFixed(0) * 5 +
        "%," +
        (l / 5).toFixed(0) * 5 +
        "%)"
    );
}

// dist in nm
function convert_nm_distance(dist, displayUnits) {
    if (displayUnits === "metric") {
        return (dist * 1.852); // nm  to kilometers
    }
    else if (displayUnits === "imperial") {
        return (dist * 1.15078); // meters to miles
    }
    return (dist); // nautical miles
}

function showLabels() {
  AllwaysShowPermanentLabels = true;
  //console.log("ASPL " + AllwaysShowPermanentLabels );
}
