"use strict";

function PlaneObject(icao) {
    // Info about the plane
    this.icao = icao;
    this.icaorange = findICAORange(icao);
    this.flight = null;
    this.squawk = null;
    this.selected = false;
    this.category = null;

    // Basic location information
    this.altitude = null;
    this.speed = null;
    this.track = null;
    this.prev_position = null;
    this.position = null;
    this.position_from_mlat = false;
    this.sitedist = null;

    // Data packet numbers
    this.messages = null;
    this.rssi = null;

    // Track history as a series of line segments
    this.elastic_feature = null;
    this.track_linesegs = [];
    this.history_size = 0;

    // When was this last updated (receiver timestamp)
    this.last_message_time = null;
    this.last_position_time = null;

    // When was this last updated (seconds before last update)
    this.seen = null;
    this.seen_pos = null;

    // Display info
    this.visible = true;
    this.marker = null;
    this.markerStyle = null;
    this.markerIcon = null;
    this.markerStaticStyle = null;
    this.markerStaticIcon = null;
    this.markerStyleKey = null;
    this.markerSvgKey = null;
    this.filter = {};

    // Akissack - additional variables for various modifications - Ref: AK9Z Start incl.
    this.is_vetted = false;
    this.is_interesting = ""; // 'Y' or not
    this.my_vet = "";         // 1 = Mil/noTrail 2 = Civ/noTrail 5 = Mil/Trail 6 = Civ/Trail 0 = ?/noTrail 3,4,7 = ERR
    this.my_trail = "";       // trail on by default
    this.ac_type = "";        // icao type
    this.ac_shortname = "";   // Short a/c name
    this.ac_aircraft = "";    // Long a/c name
    this.ac_category = "";    // My category for images -  eg 2prop
    this.ac_country = "";
    this.ac_operator = "-";
    // Akissack - additional variables for various modifications - Ref: AK9Z Ends
    this.siteBearing = 0;      // ref: AK8F
    this.siteBearingSlot = 0;  // ref: Github issue #17
    this.siteNm = 0;           // ref: AK8F
    this.fl = 0;               // ref: AK8F

    // start from a computed registration, let the DB override it
    // if it has something else.
    this.registration = registration_from_hexid(this.icao);
    this.icaotype = null;
    this.typeDescription = null;
    this.wtc = null;

    // request metadata
    getAircraftData(this.icao).done(
        function (data) {
            // console.log(data); /// aircraft data
            if ("r" in data) {
                this.registration = data.r;
            }
            if ("t" in data) {
                this.icaotype = data.t;
            }
            if ("desc" in data) {
                this.typeDescription = data.desc;
            }
            if ("wtc" in data) {
                this.wtc = data.wtc;
            }

            // -------------------------------------------------------------
            // AKISSACK - Load my details from json ------Ref: AK9E starts
            // -------------------------------------------------------------
            if (ShowMyPreferences && ShowAdditionalData) {
                if (
                    this.icao.substring(0, 3).upper == "43C" ||
                    this.icao.substring(0, 2).upper == "AE"
                ) {
                    this.is_interesting = "Y";
                    this.my_trail = true;
                }
                if ("Int" in data) {
                    if (data.Int == 1) this.is_interesting = "Y";
                }
                if ("Trail" in data) {
                    if (data.Trail == 5 || data.Trail == 6) {
                        this.my_trail = true;
                    }
                }
                if ("Country" in data) {
                    this.ac_country = data.Country;
                }
                if ("Owner" in data) {
                    this.ac_operator = data.Owner;
                }
                if ("Force" in data) {
                    this.ac_operator = data.Force;
                }
                if ("Image" in data) {
                    this.ac_category = data.Image;
                }
                if ("Short" in data) {
                    this.ac_shortname = data.Short;
                }
                if ("Type" in data) {
                    this.ac_aircraft = data.Type;
                }
            }
            // -------------------------------------------------------
            // -------------------------------------- Ref: AK9E ends
            // -------------------------------------------------------

            if (this.selected) {
                refreshSelected();
            }
        }.bind(this)
    );
}

PlaneObject.prototype.isFiltered = function () {
    // -------------------------------------------------------------
    // AKISSACK - Load my details from json ------Ref: AK11B starts
    // -------------------------------------------------------------

    var AcVisibile = true;
    var AcInBracket = true;

    if (this.filter.specials == true) {
        if (this.is_interesting != "Y") {
            AcVisibile = false;
        }
    }

    if (
        this.filter.minAltitude !== undefined &&
        this.filter.maxAltitude !== undefined
    ) {
        // we've set both heights
        if (this.altitude === null || this.altitude === undefined) {
            // if we cant get the height, let's exlcude (true)
            AcInBracket = false;
        } else {
            var planeAltitude =
                this.altitude === "ground"
                    ? 0
                    : convert_altitude(this.altitude, this.filter.altitudeUnits);
            if (
                planeAltitude < this.filter.minAltitude ||
                planeAltitude > this.filter.maxAltitude
            ) {
                AcInBracket = false;
            }
        }
        //return planeAltitude < this.filter.minAltitude || planeAltitude > this.filter.maxAltitude;  //if low or high, exclude (true)
    }

    if (AcVisibile === true && AcInBracket === true) {
        return false;
    } else {
        return true;
    }
    // -------------------------------------------------------
    // -------------------------------------- Ref: AK11B ends
    // -------------------------------------------------------
};

// Appends data to the running track so we can get a visual tail on the plane
// Only useful for a long running browser session.
PlaneObject.prototype.updateTrack = function (estimate_time) {
    if (!this.position) return false;
    if (this.position == this.prev_position) return false;

    var projHere = ol.proj.fromLonLat(this.position);
    var projPrev;
    if (this.prev_position === null) {
        projPrev = projHere;
    } else {
        projPrev = ol.proj.fromLonLat(this.prev_position);
    }

    this.prev_position = this.position;

    if (this.track_linesegs.length == 0) {
        // Brand new track
        //console.log(this.icao + " new track");
        var newseg = {
            fixed: new ol.geom.LineString([projHere]),
            feature: null,
            head_update: this.last_position_time,
            tail_update: this.last_position_time,
            estimated: false,
            ground: this.altitude === "ground",
        };
        this.track_linesegs.push(newseg);
        this.history_size++;
        return;
    }

    var lastseg = this.track_linesegs[this.track_linesegs.length - 1];
    var elapsed = this.last_position_time - lastseg.head_update;

    var est_track = elapsed > estimate_time;
    var ground_track = this.altitude === "ground";

    if (est_track) {
        if (!lastseg.estimated) {
            // >5s gap in data, create a new estimated segment
            //console.log(this.icao + " switching to estimated");
            lastseg.fixed.appendCoordinate(projPrev);
            this.track_linesegs.push({
                fixed: new ol.geom.LineString([projPrev, projHere]),
                feature: null,
                head_update: this.last_position_time,
                estimated: true,
            });
            this.history_size += 2;
        } else {
            // Keep appending to the existing dashed line; keep every point
            lastseg.fixed.appendCoordinate(projPrev);
            lastseg.head_update = this.last_position_time;
            this.history_size++;
        }

        return true;
    }

    if (lastseg.estimated) {
        // We are back to good data (we got two points close in time), switch back to
        // solid lines.
        lastseg = {
            fixed: new ol.geom.LineString([projPrev]),
            feature: null,
            head_update: this.last_position_time,
            tail_update: this.last_position_time,
            estimated: false,
            ground: this.altitude === "ground",
        };
        this.track_linesegs.push(lastseg);
        this.history_size++;
        // continue
    }

    if (
        (lastseg.ground && this.altitude !== "ground") ||
        (!lastseg.ground && this.altitude === "ground")
    ) {
        //console.log(this.icao + " ground state changed");
        // Create a new segment as the ground state changed.
        // assume the state changed halfway between the two points
        // FIXME needs reimplementing post-google

        lastseg.fixed.appendCoordinate(projPrev);
        this.track_linesegs.push({
            fixed: new ol.geom.LineString([projPrev, projHere]),
            feature: null,
            head_update: this.last_position_time,
            tail_update: this.last_position_time,
            estimated: false,
            ground: this.altitude === "ground",
        });
        this.history_size += 3;
        return true;
    }

    // Add more data to the existing track.
    // We only retain some historical points, at 5+ second intervals,
    // plus the most recent point
    if (this.last_position_time - lastseg.tail_update >= 5) {
        // enough time has elapsed; retain the last point and add a new one
        //console.log(this.icao + " retain last point");
        lastseg.fixed.appendCoordinate(projHere);
        lastseg.tail_update = lastseg.head_update;
        this.history_size++;
    }

    lastseg.head_update = this.last_position_time;
    return true;
};

// This is to remove the line from the screen if we deselect the plane
PlaneObject.prototype.clearLines = function () {
    for (var i = this.track_linesegs.length - 1; i >= 0; --i) {
        var seg = this.track_linesegs[i];
        if (seg.feature !== null) {
            PlaneTrailFeatures.remove(seg.feature);
            seg.feature = null;
        }
    }

    if (this.elastic_feature !== null) {
        PlaneTrailFeatures.remove(this.elastic_feature);
        this.elastic_feature = null;
    }
};

PlaneObject.prototype.getDataSource = function () {
    // MLAT
    if (this.position_from_mlat) {
        return "mlat";
    }

    // Not MLAT, but position reported - ADSB or variants
    if (this.position !== null) {
        return this.addrtype;
    }

    // Otherwise Mode S
    return "mode_s";

    // TODO: add support for Mode A/C
};

PlaneObject.prototype.getMarkerColor = function () {
    // Emergency squawks override everything else
    if (this.squawk in SpecialSquawks)
        return SpecialSquawks[this.squawk].markerColor;

    var h, s, l;

    if (this.altitude === null) {
        h = ColorByAlt.unknown.h;
        s = ColorByAlt.unknown.s;
        l = ColorByAlt.unknown.l;
    } else if (this.altitude === "ground") {
        h = ColorByAlt.ground.h;
        s = ColorByAlt.ground.s;
        l = ColorByAlt.ground.l;
    } else {
        s = ColorByAlt.air.s;
        l = ColorByAlt.air.l;

        // find the pair of points the current altitude lies between,
        // and interpolate the hue between those points
        var hpoints = ColorByAlt.air.h;
        h = hpoints[0].val;
        for (var i = hpoints.length - 1; i >= 0; --i) {
            if (this.altitude > hpoints[i].alt) {
                if (i == hpoints.length - 1) {
                    h = hpoints[i].val;
                } else {
                    h =
                        hpoints[i].val +
                        ((hpoints[i + 1].val - hpoints[i].val) *
                            (this.altitude - hpoints[i].alt)) /
                        (hpoints[i + 1].alt - hpoints[i].alt);
                }
                break;
            }
        }
    }

    // If we have not seen a recent position update, change color
    if (this.seen_pos > 15) {
        h += ColorByAlt.stale.h;
        s += ColorByAlt.stale.s;
        l += ColorByAlt.stale.l;
    }

    // If this marker is selected, change color
    if (this.selected && !SelectedAllPlanes) {
        h += ColorByAlt.selected.h;
        s += ColorByAlt.selected.s;
        l += ColorByAlt.selected.l;
    }

    // If this marker is a mlat position, change color
    if (this.position_from_mlat) {
        h += ColorByAlt.mlat.h;
        s += ColorByAlt.mlat.s;
        l += ColorByAlt.mlat.l;
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

    // ---------------------------   AKISSACK mono colour  Ref: AK9C Start
    if (ShowMyPreferences && ShowSimpleColours) {
        var myColour = IsDarkMap ? "#0000FF" : "#333399";
        if (this.is_interesting) {
            myColour = IsDarkMap ? "#FF0000" : "#993333";
        }
        return myColour;
    } else {
        // ---------------------------   AKISSACK mono colour  Ref: AK9C ends
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
};

PlaneObject.prototype.updateIcon = function () {
    var scaleFactor = Math.max(
        0.2,
        Math.min(1.2, 0.15 * Math.pow(1.25, ZoomLvl))
    ).toFixed(1);

    var col = this.getMarkerColor();
    var opacity = 1.0;
    var outline = this.position_from_mlat ? OutlineMlatColor : OutlineADSBColor;
    var baseMarker = getBaseMarker(
        this.category,
        this.icaotype,
        this.typeDescription,
        this.wtc
    );
    if (ShowMyPreferences) {
        // Ref: AK9D starts
        if (ShowMyIcons) {
            // Ref: AK10B starts
            var baseMarker = getMyBaseMarker(
                this.category,
                this.icaotype,
                this.typeDescription,
                this.wtc,
                this.ac_category
            );
        } // Ref: AK10B ends
        var adjWeight = this.is_interesting ? 0.5 : 0.5;
        var weight = (
            (this.selected ? 0.75 : adjWeight) / baseMarker.scale
        ).toFixed(1);
        if (this.is_interesting == "Y") {
            outline = "rgb(128, 0, 0)";
        }
    } else {
        var weight = (
            (this.selected && !SelectedAllPlanes ? 2 : 1) /
            baseMarker.scale /
            scaleFactor
        ).toFixed(1);
    } //Ref: AK9D ends

    var rotation = this.track === null ? 0 : this.track;
    var transparentBorderWidth = (32 / baseMarker.scale / scaleFactor).toFixed(1);

    var svgKey =
        col +
        "!" +
        outline +
        "!" +
        baseMarker.key +
        "!" +
        weight +
        "!" +
        scaleFactor;
    var styleKey = opacity + "!" + rotation;

    if (
        this.markerStyle === null ||
        this.markerIcon === null ||
        this.markerSvgKey != svgKey
    ) {
        //console.log(this.icao + " new icon and style " + this.markerSvgKey + " -> " + svgKey);

        var icon = new ol.style.Icon({
            anchor: baseMarker.anchor,
            anchorXUnits: "pixels",
            anchorYUnits: "pixels",
            scale: baseMarker.scale * scaleFactor,
            imgSize: baseMarker.size,
            src: svgPathToURI(
                baseMarker.path,
                baseMarker.size,
                outline,
                weight,
                col,
                transparentBorderWidth
            ),
            rotation: baseMarker.noRotate ? 0 : (rotation * Math.PI) / 180.0,
            opacity: opacity,
            rotateWithView: baseMarker.noRotate ? false : true,
        });

        if (baseMarker.noRotate) {
            // the base marker won't be rotated
            this.markerStaticIcon = icon;
            this.markerStaticStyle = new ol.style.Style({
                image: this.markerStaticIcon,
            });

            // create an arrow that we will rotate around the base marker
            // to indicate heading

            var offset = baseMarker.markerRadius * baseMarker.scale + 6;
            var size = offset * 2;

            var arrowPath = "M " + offset + ",0 m 4,4 -8,0 4,-4 z";
            this.markerIcon = new ol.style.Icon({
                anchor: [offset, offset],
                anchorXUnits: "pixels",
                anchorYUnits: "pixels",
                scale: 1.0 * scaleFactor,
                imgSize: [size, size],
                src: svgPathToURI(arrowPath, [size, size], outline, 1, outline, 0),
                rotation: (rotation * Math.PI) / 180.0,
                opacity: opacity,
                rotateWithView: true,
            });
            this.markerStyle = new ol.style.Style({
                image: this.markerIcon,
            });
        } else {
            this.markerIcon = icon;

            // ---------------------------------------------------------------------------------------
            // AKISSACK - PERMANENT LABELS - Part 1 ----------------------------------------- ref: AK7A
            // ---------------------------------------------------------------------------------------
            if (ShowPermanentLabels) {
                var labelText = "";
                this.markerStyle = new ol.style.Style({
                    text: new ol.style.Text({
                        text: labelText,
                        fill: new ol.style.Fill({ color: "blue" }),
                        stroke: new ol.style.Stroke({ color: "yellow", width: 7 }),
                        textAlign: "left",
                        textBaseline: "bottom",
                        font: "normal 10px tahoma",
                        offsetX: +15,
                        offsetY: +30,
                    }),
                    image: this.markerIcon,
                });
            } else {
                this.markerStyle = new ol.style.Style({
                    image: this.markerIcon,
                });
            }
            // ---------------------------------------------------------------------------------------
            // ----------------------------------------------------------------------------- AKISSACK
            // ---------------------------------------------------------------------------------------

            this.markerStaticIcon = null;
            this.markerStaticStyle = new ol.style.Style({});
        }

        this.markerStyleKey = styleKey;
        this.markerSvgKey = svgKey;

        if (this.marker !== null) {
            this.marker.setStyle(this.markerStyle);
            this.markerStatic.setStyle(this.markerStaticStyle);
        }
    }

    if (this.markerStyleKey != styleKey) {
        //console.log(this.icao + " new rotation");
        this.markerIcon.setRotation((rotation * Math.PI) / 180.0);
        this.markerIcon.setOpacity(opacity);
        if (this.staticIcon) {
            this.staticIcon.setOpacity(opacity);
        }
        this.markerStyleKey = styleKey;
    }

    return true;
};

// Update our data
PlaneObject.prototype.updateData = function (receiver_timestamp, data) {
    // Update all of our data
    this.messages = data.messages;
    this.rssi = data.rssi;
    this.last_message_time = receiver_timestamp - data.seen;

    if (typeof data.type !== "undefined") this.addrtype = data.type;
    else this.addrtype = "adsb_icao";

    // AKISSACK - Altitude taking into account differering data structures (changed in FA 3.6.2?)
    if (typeof data.altitude !== "undefined") {
        this.altitude = data.altitude;
    } else if (typeof data.alt_baro !== "undefined") {
        this.altitude = data.alt_baro;
    } else if (typeof data.alt_geom !== "undefined") {
        this.altitude = data.alt_geom;
    }
    if (typeof this.altitude !== "undefined") {
        this.fl = parseInt(this.altitude / 100);
    }

    // AKISSACK - Rate of climb/descent taking into account differering data structures (changed in FA 3.6.2?)
    if (typeof data.vert_rate !== "undefined") {
        this.vert_rate = data.vert_rate;
    } else if (typeof data.geom_rate !== "undefined") {
        this.vert_rate = data.geom_rate;
    } else if (typeof data.baro_rate !== "undefined") {
        this.vert_rate = data.baro_rate;
    }

    // AKISSACK - Speed taking into account differering data structures (changed in FA 3.6.2?)
    if (typeof data.speed !== "undefined") {
        this.speed = data.speed;
    } else if (typeof data.gs !== "undefined") {
        this.speed = data.gs;
    } else if (typeof data.tas !== "undefined") {
        this.speed = data.tas;
    } else if (typeof data.ias !== "undefined") {
        this.speed = data.ias;
    }

    if (typeof data.track !== "undefined") this.track = data.track;

    if (typeof data.lat !== "undefined") {
        this.position = [data.lon, data.lat];
        this.last_position_time = receiver_timestamp - data.seen_pos;

        if (SitePosition !== null) {
            //var WGS84 = new ol.Sphere(6378137);
            //this.sitedist = WGS84.haversineDistance(SitePosition, this.position);
            //  AKISSACK     - Store a bearing and nm distance for our range plot            Ref AK8G
            this.sitedist = ol.sphere.getDistance(SitePosition, this.position);

            //  increased the resolution from one degree to half-degree  // ref: Github issue #17

            this.siteBearing = getBearing(
                    SitePosition[1],
                    SitePosition[0],
                    this.position[1],
                    this.position[0]
            ).toFixed(1);

            this.siteBearingSlot = Math.round(2 * this.siteBearing);
            if (this.siteBearingSlot == 720) this.siteBearingSlot=0;

            //this.siteBearingSlot = Math.round(2 * getBearing(
            //        SitePosition[1],
            //        SitePosition[0],
            //        this.position[1],
            //        this.position[0]
            //).toFixed(1));
	    
            //this.siteBearing = parseInt(
            //    getBearing(
            //        SitePosition[1],
            //        SitePosition[0],
            //        this.position[1],
            //        this.position[0]
            //    ).toFixed(0)
            //);

            this.siteNm = parseInt((this.sitedist / 1852).toFixed(1));

            //if (this.icao.substring(0,5) == "43c74"){
            //  console.log(this.icao+" bearing: "+this.siteBearing+" slot: "+this.siteBearingSlot +" , range: "+this.siteNm);
            //}
        }

        this.position_from_mlat = false;
        if (typeof data.mlat !== "undefined") {
            for (var i = 0; i < data.mlat.length; ++i) {
                if (data.mlat[i] === "lat" || data.mlat[i] == "lon") {
                    this.position_from_mlat = true;
                    break;
                }
            }
        }
    }
    if (typeof data.flight !== "undefined") this.flight = data.flight;
    if (typeof data.squawk !== "undefined") this.squawk = data.squawk;
    if (typeof data.category !== "undefined") this.category = data.category;
};

PlaneObject.prototype.updateTick = function (
    receiver_timestamp,
    last_timestamp
) {
    // recompute seen and seen_pos
    this.seen = receiver_timestamp - this.last_message_time;
    this.seen_pos =
        this.last_position_time === null
            ? null
            : receiver_timestamp - this.last_position_time;

    // If no packet in over 58 seconds, clear the plane.
    if (this.seen > 60) {
        if (this.visible) {
            //console.log("hiding " + this.icao);
            this.clearMarker();
            this.visible = false;
            if (SelectedPlane == this.icao) selectPlaneByHex(null, false);
        }
    } else {
        // if (this.position !== null && (this.selected || this.seen_pos < 60)) {
        if (this.position !== null && (this.selected || this.seen < 60)) {
            this.visible = true;
            if (
                this.updateTrack(
                    receiver_timestamp -
                    last_timestamp +
                    (this.position_from_mlat ? 30 : 5)
                )
            ) {
                this.updateLines();
                this.updateMarker(true);

                // ------------------------------------------------
                // AKISSACK store range plot details  Ref AK8H
                // ------------------------------------------------

                // MINUMUM RANGE RINGS - MinRangeHeight is set in config.js as the upper bound for this ring
                //                     - MinRangeLikely is set in config.js and is maximum likely distance for MinRangeHeight    
                if (this.altitude <= MinRangeHeight) {
                    if (MinRngRange[this.siteBearingSlot] < this.siteNm && this.siteNm < MinRangeLikely) {  // Update sessionStorage and also update MariaDb if in use
                        MinRngRange[this.siteBearingSlot] = this.siteNm;
                        MinRngLat[this.siteBearingSlot] = this.position[1];
                        MinRngLon[this.siteBearingSlot] = this.position[0];
                        if (TypeOfStorageSession == 'Session') {
                            sessionStorage.setItem("MinRngRange", JSON.stringify(MinRngRange));
                            sessionStorage.setItem("MinRngLat", JSON.stringify(MinRngLat));
                            sessionStorage.setItem("MinRngLon", JSON.stringify(MinRngLon));
                        } else {
                            localStorage.setItem("MinRngRange", JSON.stringify(MinRngRange));
                            localStorage.setItem("MinRngLat", JSON.stringify(MinRngLat));
                            localStorage.setItem("MinRngLon", JSON.stringify(MinRngLon));
                        }
                        //console.log("slot: "+this.siteBearingSlot+" "+("000" + this.siteBearing).slice(-5) + "° " + ("000" + this.siteNm).slice(-3)+"nm logged as MIN range.");
                        if (SleafordMySql) {
                            //console.log(("000" + this.siteBearingSlot).slice(-3) + "° " + ("000" + this.siteNm).slice(-3)+"nm logged as MID "+this.fl);
                            updateMySqlRange(
                                "min",
                                this.siteBearing,
                                this.siteNm,
                                this.position[1],
                                this.position[0],
                                this.icao,
                                this.fl
                            );
                        }
                    }
                }

                // MIDDLE RANGE RINGS  - MidRangeHeight is set in config.js as the upper bound for this ring
                //                     - MidRangeLikely is set in config.js and is maximum likely distance for MidRangeHeight    
                if (this.altitude <= MidRangeHeight) {
                    if (MidRngRange[this.siteBearingSlot] < this.siteNm && this.siteNm < MidRangeLikely) {
                        MidRngRange[this.siteBearingSlot] = this.siteNm;
                        MidRngLat[this.siteBearingSlot] = this.position[1];
                        MidRngLon[this.siteBearingSlot] = this.position[0];
                        if (TypeOfStorageSession == 'Session') {
                            sessionStorage.setItem("MidRngRange", JSON.stringify(MidRngRange));
                            sessionStorage.setItem("MidRngLat", JSON.stringify(MidRngLat));
                            sessionStorage.setItem("MidRngLon", JSON.stringify(MidRngLon));
                        } else {
                            localStorage.setItem("MidRngRange", JSON.stringify(MidRngRange));
                            localStorage.setItem("MidRngLat", JSON.stringify(MidRngLat));
                            localStorage.setItem("MidRngLon", JSON.stringify(MidRngLon));
                        }
                        //console.log("slot: "+this.siteBearingSlot+" "+("000" + this.siteBearing).slice(-5) + "° " + ("000" + this.siteNm).slice(-3)+"nm logged as MID range.");
                        if (SleafordMySql) {
                            //console.log(("000" + this.siteBearing).slice(-3) + "° " + ("000" + this.siteNm).slice(-3)+"nm logged as MID "+this.altitude);
                            updateMySqlRange(
                                "mid",
                                this.siteBearing,
                                this.siteNm,
                                this.position[1],
                                this.position[0],
                                this.icao,
                                this.fl
                            );
                        }
                    }
                }

                // MAXIMUM RANGE RINGS - MaxRangeHeight is set in config.js as the upper bound for this ring. 
                //                     - MaxRangeLikely is set in config.js and is maximum likely distance for MaxRangeHeight   
                //                     - These are theoretical absolute maximums to weed out bad plots		
                if (this.altitude <= MaxRangeHeight) {
                    if (MaxRngRange[this.siteBearingSlot] < this.siteNm && this.siteNm < MaxRangeLikely) {
                        MaxRngRange[this.siteBearingSlot] = this.siteNm;
                        MaxRngLat[this.siteBearingSlot] = this.position[1];
                        MaxRngLon[this.siteBearingSlot] = this.position[0];
                        if (TypeOfStorageSession == 'Session') {
                            sessionStorage.setItem("MaxRngRange", JSON.stringify(MaxRngRange));
                            sessionStorage.setItem("MaxRngLat", JSON.stringify(MaxRngLat));
                            sessionStorage.setItem("MaxRngLon", JSON.stringify(MaxRngLon));
                        } else {
                            localStorage.setItem("MaxRngRange", JSON.stringify(MaxRngRange));
                            localStorage.setItem("MaxRngLat", JSON.stringify(MaxRngLat));
                            localStorage.setItem("MaxRngLon", JSON.stringify(MaxRngLon));
                        }
                        //console.log("slot: "+this.siteBearingSlot+" "+("000" + this.siteBearing).slice(-5) + "° " + ("000" + this.siteNm).slice(-3)+"nm logged as MAX range.");
                        if (SleafordMySql) {
                            //console.log(("000" + this.siteBearing).slice(-3) + "° " + ("000" + this.siteNm).slice(-3)+"nm logged as MAX "+this.altitude);
                            updateMySqlRange(
                                "max",
                                this.siteBearing,
                                this.siteNm,
                                this.position[1],
                                this.position[0],
                                this.icao,
                                this.fl
                            );
                        }
                    }
                }

            } else {
                this.updateMarker(false); // didn't move
            }
        } else {
            this.clearMarker();
            this.visible = false;
        }
    }
};

PlaneObject.prototype.clearMarker = function () {
    if (this.marker) {
        PlaneIconFeatures.remove(this.marker);
        PlaneIconFeatures.remove(this.markerStatic);
        /* FIXME google.maps.event.clearListeners(this.marker, 'click'); */
        this.marker = this.markerStatic = null;
    }
};

// Update our marker on the map
PlaneObject.prototype.updateMarker = function (moved) {
    if (!this.visible || this.position == null || this.isFiltered()) {
        this.clearMarker();
        return;
    }

    this.updateIcon();
    if (this.marker) {
        if (moved) {
            this.marker.setGeometry(
                new ol.geom.Point(ol.proj.fromLonLat(this.position))
            );

            // ---------------------------------------------------------------------
            // AKISSACK - PERMANENT LABEL PART 2 - Update ---------------- ref: AK7A
            // ---------------------------------------------------------------------
            //console.log("ASPL " + AllwaysShowPermanentLabels );
            if (ShowPermanentLabels) {
                // Update label as as well as moving we may have gone up or down
                var v = ""; // An indication of level, climbing or decending
                var labelText = "";
                this.labelColour = "#ffffff";

                if (ZoomLvl > 10 || AllwaysShowPermanentLabels ) {
                    if (this.vert_rate > 256) {
                        v = UP_TRIANGLE;
                    } else {
                        if (this.vert_rate < -256) {
                            v = DOWN_TRIANGLE;
                        }
                    }
  
                    if (IsDarkMap) {
                        // LINE ONE
                         if (this.flight) {
                             labelText = labelText + this.flight;
                        } else {
                             if (this.registration) {labelText = labelText + this.registration};
                        }
                        labelText = labelText + " [" + (this.squawk ? this.squawk : "No Squawk ") + "]";
                        if (this.selected && !SelectedAllPlanes) {
                            this.labelColour = "#ffff00"; 
                        } else this.labelColour = "#ffffff"; 

                        //LINE TWO
                        var hgt = parseInt(this.fl ? (this.fl) : 0);
                        hgt = convert_altitude(hgt * 100, DisplayUnits);
                        labelText = labelText + "\n" + parseInt(hgt) + (DisplayUnits === "metric" ? "m" : " ft") + v;
                    } else {
                        // LINE ONE
                        labelText = this.flight ? this.flight : "No Ident ";
                        if (this.registration) {labelText = labelText + this.registration;}
                        labelText = labelText + (this.squawk ? " [" + this.squawk + "]" : "");
                        if (this.selected && !SelectedAllPlanes) {
                            this.labelColour = "#ffff00"; 
                        } else this.labelColour = "#ffffff"; 

                        //LINE TWO
                        if (ShowAdditionalData) {
                            //  Let's try an alternative to ID -> https://github.com/alkissack/Dump1090-OpenLayers3-html/issues/3
                            var tmpText = this.ac_aircraft ? this.ac_aircraft : "-";
                            if (tmpText === "-") {tmpText = this.icaotype ? this.icaotype : "Unknown Type";}
                            labelText = labelText + "\n" + tmpText;
                        }

                        //LINE THREE
                        labelText = labelText + "\n" + this.icao.toUpperCase();
                        var hgt = parseInt(this.fl ? (this.fl) : 0);
                        hgt = convert_altitude(hgt * 100, DisplayUnits);
                        labelText = labelText + "[" + parseInt(hgt) + (DisplayUnits === "metric" ? "m" : " ft") + "]";
                    }
                }

                var hexColour = this.labelColour; // New section for semi transparency
                var myStrokeColour = ol.color.asArray(hexColour);
                myStrokeColour = myStrokeColour.slice();
                myStrokeColour[3] = this.selected ? (IsDarkMap ? 1 : 0.5) : (IsDarkMap ? 1 : 0.25) ;  // change alpha
                if (ShowAdditionalData) {
                    hexColour = this.is_interesting ? (IsDarkMap ? "#aa0000" : "#ff0000") : (IsDarkMap ? "#6666ff" : "#0000ff") ;
                } else {
                    hexColour = (IsDarkMap ? "#0000aa" : "#0000ff");
                }

                var myFillColour = ol.color.asArray(hexColour);
                myFillColour = myFillColour.slice();
                myFillColour[3] = 1; //this.selected ? 1 : 1;

                var newS = new ol.style.Style({
                    text: new ol.style.Text({
                        text: labelText,
                        fill: new ol.style.Fill({
                            color: myFillColour,
                        }),
                        stroke: new ol.style.Stroke({
                            color: (IsDarkMap ? "transparent" : myStrokeColour), 
                            width: (IsDarkMap ? 2 : 2),
                        }),
                        textAlign: "left",
                        textBaseline: "bottom",
                        font: (IsDarkMap ? "bold  11px tahoma" : "normal 9px tahoma"),
                        offsetX: +25,
                        offsetY: +15,
                    }),
                    image: this.markerIcon,
                });
                this.marker.setStyle(newS);
            }
            // ----------------------------------------------------------------------------------
            // ------------------------------------------------------------------------- AKISSACK
            // ----------------------------------------------------------------------------------

            this.markerStatic.setGeometry(
                new ol.geom.Point(ol.proj.fromLonLat(this.position))
            );
        }
    } else {
        // ----------------------------------------------------------------------------------
        // AKISSACK - HOVER OVER LABELS ------------------------------------ ref: AK6A starts
        // ----------------------------------------------------------------------------------
        if (ShowHoverOverLabels) {
            var myPopUpName = "~"; // Set a default name
            this.marker = new ol.Feature({
                geometry: new ol.geom.Point(ol.proj.fromLonLat(this.position)),
                name: myPopUpName,
            });
        } else {
            this.marker = new ol.Feature(
                new ol.geom.Point(ol.proj.fromLonLat(this.position))
            );
        }
        // ----------------------------------------------------------------------------------
        // ------------------------------------------------------------------- ref: AK6A ends
        // ----------------------------------------------------------------------------------

        this.marker.hex = this.icao;
        this.marker.setStyle(this.markerStyle);
        PlaneIconFeatures.push(this.marker);

        this.markerStatic = new ol.Feature(
            new ol.geom.Point(ol.proj.fromLonLat(this.position))
        );
        this.markerStatic.hex = this.icao;
        this.markerStatic.setStyle(this.markerStaticStyle);
        PlaneIconFeatures.push(this.markerStatic);
    }
};

// Update our planes tail line,
PlaneObject.prototype.updateLines = function () {
    if (!this.selected) return;

    if (this.track_linesegs.length == 0) return;

    var estimateStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({
            color: "#a08080",
            width: 0.75, // Reduced width Ref: AK9A
            lineDash: [3, 3],
        }),
    });

    if (ShowMyPreferences) {
        // AKISSACK
        var airStyle = new ol.style.Style({
            stroke: new ol.style.Stroke({
                color: this.is_interesting == "Y" ? "#ff0000" : "#0000ff",
                width: 0.75, // Reduced width Ref: AK9A
            }),
        });
    } else {
        var airStyle = new ol.style.Style({
            stroke: new ol.style.Stroke({
                color: "#000000",
                width: 0.75, // Reduced width Ref: AK9A
            }),
        });
    }

    var groundStyle = new ol.style.Style({
        stroke: new ol.style.Stroke({
            color: "#408040",
            width: 0.75, // Reduced width Ref: AK9A
        }),
    });

    // find the old elastic band so we can replace it in place
    // (which should be faster than remove-and-add when PlaneTrailFeatures is large)
    var oldElastic = -1;
    if (this.elastic_feature !== null) {
        oldElastic = PlaneTrailFeatures.getArray().indexOf(this.elastic_feature);
    }

    // create the new elastic band feature
    var lastseg = this.track_linesegs[this.track_linesegs.length - 1];
    var lastfixed = lastseg.fixed.getCoordinateAt(1.0);
    var geom = new ol.geom.LineString([
        lastfixed,
        ol.proj.fromLonLat(this.position),
    ]);
    this.elastic_feature = new ol.Feature(geom);
    this.elastic_feature.setStyle(
        this.altitude === "ground" ? groundStyle : airStyle
    );

    if (oldElastic < 0) {
        PlaneTrailFeatures.push(this.elastic_feature);
    } else {
        PlaneTrailFeatures.setAt(oldElastic, this.elastic_feature);
    }

    // create any missing fixed line features
    for (var i = 0; i < this.track_linesegs.length; ++i) {
        var seg = this.track_linesegs[i];
        if (seg.feature === null) {
            seg.feature = new ol.Feature(seg.fixed);
            if (seg.estimated) {
                seg.feature.setStyle(estimateStyle);
            } else if (seg.ground) {
                seg.feature.setStyle(groundStyle);
            } else {
                seg.feature.setStyle(airStyle);
            }
            PlaneTrailFeatures.push(seg.feature);
        }
    }
};

PlaneObject.prototype.destroy = function () {
    this.clearLines();
    this.clearMarker();
};

// AKISSACK Ref: AK8I
function radians(n) {
    return n * (Math.PI / 180);
}

function degrees(n) {  //degrees to radians
    return n * (180 / Math.PI);
}

// AKISSACK
function getBearing(startLat, startLong, endLat, endLong) {
    startLat = radians(startLat);
    startLong = radians(startLong);
    endLat = radians(endLat);
    endLong = radians(endLong);

    var dLong = endLong - startLong;

    var dPhi = Math.log(
        Math.tan(endLat / 2.0 + Math.PI / 4.0) /
        Math.tan(startLat / 2.0 + Math.PI / 4.0)
    );
    if (Math.abs(dLong) > Math.PI) {
        if (dLong > 0.0) dLong = -(2.0 * Math.PI - dLong);
        else dLong = 2.0 * Math.PI + dLong;
    };

    return (degrees(Math.atan2(dLong, dPhi)) + 360.0) % 360.0;
}

function updateMySqlRange(ring, bearing, dist, lat, lon, icao, fl) {
    //   updateMySqlRange("max", this.siteBearing, this.siteNm, this.position[1], this.position[0], this.icao, this.fl );

    icao = icao.toUpperCase();
    ring = ring.toLowerCase();

    if (bearing === 360) bearing = 0;

    //if (dist > 0) console.log(("000" + bearing).slice(-3) + "° " + ("000" + dist).slice(-3)+"nm "+ icao);
    //console.log(("000" + bearing).slice(-3) + "° " + ("000" + dist).slice(-3)+"nm FL:"+("000" + fl).slice(-3)+" "+ring+" "+ icao);

    $(function () {
        $.ajax({
            url: "sql/range_update_one_3.php",
            async: true,
            data:
                "ring=" + ring +
                "&bearing=" + bearing +
                "&range=" + dist +
                "&lat=" + lat +
                "&lon=" + lon +
                "&icao=" + icao +
                "&fltlvl=" + fl,
            dataType: "json",
            success: function (data) {
                //  never saw a success message, not sure why
                //var successmessage = '*** SQL Range Data was succesfully saved';
                //console.log("*** ")+data;
            },
            error: function (data) {
                //var errormessage = 'SQL Range Data wasnt saved';
                //console.log("ERR "+data);
            },
        });
    });
}
