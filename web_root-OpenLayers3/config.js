// --------------------------------------------------------
//
// This file is to configure the configurable settings.
// Load this file before script.js file at gmap.html.
//
// --------------------------------------------------------

// -- Title Settings --------------------------------------
// Show number of aircraft and/or messages per second in the page title
PlaneCountInTitle = true;
MessageRateInTitle = false;

// -- Output Settings -------------------------------------
// The DisplayUnits setting controls whether nautical (ft, NM, knots),
// metric (m, km, km/h) or imperial (ft, mi, mph) units are used in the
// plane table and in the detailed plane info. Valid values are
// "nautical", "metric", or "imperial".
DisplayUnits = "nautical";

// -- Map settings ----------------------------------------
// These settings are overridden by any position information
// provided by dump1090 itself. All positions are in decimal
// degrees.

// The google maps zoom level, 0 - 16, lower is further out
DefaultZoomLvl = 7;

// Center marker. If dump1090 provides a receiver location,
// that location is used and these settings are ignored.

SiteShow = true;	// true to show a center marker
SiteLat  = 53;		// position of the marker
SiteLon  = -0.4;	// *****  CHANGE THE LAT/LONG to match your location *****
SiteName = "Rx";	// tooltip of the marker

// Default center of the map.
DefaultCenterLat = SiteLat;
DefaultCenterLon = SiteLon;

// -- Marker settings -------------------------------------

// These settings control the coloring of aircraft by altitude.
// All color values are given as Hue (0-359) / Saturation (0-100) / Lightness (0-100)
ColorByAlt = {
    // HSL for planes with unknown altitude:
    unknown: { h: 0, s: 0, l: 40 },

    // HSL for planes that are on the ground:
    ground: { h: 120, s: 100, l: 30 },

    air: {
        // These define altitude-to-hue mappings
        // at particular altitudes; the hue
        // for intermediate altitudes that lie
        // between the provided altitudes is linearly
        // interpolated.
        //
        // Mappings must be provided in increasing
        // order of altitude.
        //
        // Altitudes below the first entry use the
        // hue of the first entry; altitudes above
        // the last entry use the hue of the last
        // entry.
        h: [{ alt: 2000, val: 20 },    // orange
        { alt: 10000, val: 140 },   // light green
        { alt: 40000, val: 300 }], // magenta
        s: 85,
        l: 50,
    },

    // Changes added to the color of the currently selected plane
    selected: { h: 0, s: -10, l: +20 },

    // Changes added to the color of planes that have stale position info
    stale: { h: 0, s: -10, l: +30 },

    // Changes added to the color of planes that have positions from mlat
    mlat: { h: 0, s: -10, l: -10 }
};

// For a monochrome display try this:
// ColorByAlt = {
//         unknown :  { h: 0, s: 0, l: 40 },
//         ground  :  { h: 0, s: 0, l: 30 },
//         air :      { h: [ { alt: 0, val: 0 } ], s: 0, l: 50 },
//         selected : { h: 0, s: 0, l: +30 },
//         stale :    { h: 0, s: 0, l: +30 },
//         mlat :     { h: 0, s: 0, l: -10 }
// };

// Outline color for aircraft icons with an ADS-B position
OutlineADSBColor = '#000000';

// Outline color for aircraft icons with a mlat position
OutlineMlatColor = '#4040FF';

SiteCircles = true; // true to show circles (only shown if the center marker is shown)
// In miles, nautical miles, or km (depending settings value 'DisplayUnits')
SiteCirclesDistances = new Array(100, 150, 200, 250);

// Controls page title, righthand pane when nothing is selected
PageName = "FlightAware";

// Show country flags by ICAO addresses?
ShowFlags = true;

// Path to country flags (can be a relative or absolute URL; include a trailing /)
FlagPath = "flags-tiny/";

// Set to true to enable the ChartBundle base layers (US coverage only)
ChartBundleLayers = true;

// Provide a Bing Maps API key here to enable the Bing imagery layer.
// You can obtain a free key (with usage limits) at
// https://www.bingmapsportal.com/ (you need a "basic key")
//
// Be sure to quote your key:
//   BingMapsAPIKey = "your key here";
//
BingMapsAPIKey = "";

// Provide a Mapzen API key here to enable the Mapzen vector tile layer.
// You can obtain a free key at https://mapzen.com/developers/
// (you need a "vector tiles" key)
//
// Be sure to quote your key:
//   MapzenAPIKey = "your key here";
//
MapzenAPIKey = null;

// Provide a OpenAIP API key here to enable the OpenAIP vector tile layer.
// You can obtain a free key after registering, 
// See https://docs.openaip.net/?urls.primaryName=Tiles%20API
//
// Be sure to quote your key:
//   OpenAipAPIKey = "your key here";
//
OpenAIPAPIKey = null;


UseDefaultTerrainRings = true;    // default Terrian rings color, otherwise colored by altitude (color defined in TerrainColorByAlt)
UseTerrainLineDash = true;        // true: dashed or false: solid terrain rings
TerrainLineWidth = 1;             // line width of terrain rings
TerrainAltitudes = [9842, 39370]; // altitudes in ft as in alt parameter TerrainColorByAlt, replace XXXXXXX with your code: sudo wget -O /usr/share/dump1090-fa/html/upintheair.json "www.heywhatsthat.com/api/upintheair.json?id=XXXXXXX&refraction=0.25&alts=3000,12000" 
TerrainColorByAlt = {             // colours depending on altitude (UseDefaultTerrainRings must be false and TerrainAltitudes must be set), default same as colours of planes in air, alt in ft
    h: [{ alt: 2000, val: 20 },   // orange
    { alt: 10000, val: 140 },     // light green
    { alt: 40000, val: 300 }],    // magenta
    s: 85,
    l: 50,
};

ShowSiteRingDistanceText = true;  // show the distance text in site rings

UseJetPhotosPhotoLink = false;    // Use jetphotos.com instead of FlightAware for photo links

// for this you have to change /etc/lighttpd/conf-enabled/89-dump1090-fa.conf : commenting out the filter $HTTP["url"] =~ "^/dump1090-fa/data/.*\.json$"  and always send the response header
// maybe filter is not correct --- Help wanted
// the last 3 lines should look like this without the //
// #$HTTP["url"] =~ "^/dump1090-fa/data/.*\.json$" {
//       setenv.add-response-header = ( "Access-Control-Allow-Origin" => "*" )
// #}

EndpointDump1090 = "";    // insert here endpoint to other computer where dump1090 is running (ex: http://192.168.1.152:8080/), leave it empty if it is running here

// ----------------------------------------------------------------------------------------------------------------------------
// Options to enable/disable modifications provided in Dump1090-OpenLayers3-html by Al Kissack
// ----------------------------------------------------------------------------------------------------------------------------
ShowMouseLatLong = true;     // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/1.-Mouse-position-Latitude-and-Longitude
ShowAdditionalMaps = true;   // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/2.-Additional-maps
ShowPermanentLabels = true;  // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/7.-Permanent-labels
ShowHoverOverLabels = true;  // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/6.-Hover-over-labels
ShowRanges = true;           // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/8.-Maximum-range-plot

// If showing ranges, set SiteLat/SiteLon as these are the zero range positions till plot is drawn
MinRangeHeight = 2000;   // ft - inner range ring - Set -1 to disable collection and display
MinRangeLikely = 75;     // nm - practical max (to supress spikes from bad data)
MinRangeShow = true;     // set to show min range currently captured (assuming MinRangeHeight is set too)

MidRangeHeight = 10000; // ft - mid range ring - Set -1 to disable collection and display
MidRangeLikely = 125;   // nm - practical max
MidRangeShow = true;    // set to show mid range currently captured (assuming MidRangeHeight is set too)

MaxRangeHeight = 99999;
MaxRangeLikely = 300; // nm - practical max 300
MaxRangeShow = true;  // set to show max range currently captured 

RangeLine = 1;        // Line width for range plot

TypeOfStorageSession = 'Local';  // Local or Session - Session applies per browser session, Local persists for the browser if closed and reopened.

// ----------------------------------------------------------------------------------------------------------------------------
//           UK ONLY :
// ----------------------------------------------------------------------------------------------------------------------------
ShowUSLayers = false;     // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/3.-US-Layers
ShowUKCivviLayers = true; // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/4.-UK-Civilian-overlays
ShowUKMilLayers = true;   // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/5.-UK-Military-overlays
ShowAirfieldNames = true; // show airfield name when runway is selected.
// ----------------------------------------------------------------------------------------------------------------------------
//           PERSONAL OPTIONS      https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/9.-Minor-personal-preference-changes
// ----------------------------------------------------------------------------------------------------------------------------
ShowMyPreferences = true;  // Required to enable the FOUR options below
ShowAdditionalData = true; 
ShowMyIcons = true;        // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/10.-Aircraft-icon-changes
ShowSimpleColours = true;  // https://github.com/alkissack/Dump1090-OpenLayers3-html/wiki/9.-Minor-personal-preference-changes
// ******************************************************************************
ShowHTMLColumns = true;   // *** If you turn this off, use the original-index.html file instead         ***
// ******************************************************************************
// ----------------------------------------------------------------------------------------------------------------------------
//           PRIVATE OPTIONS
// ----------------------------------------------------------------------------------------------------------------------------
ShowMyFindsLayer = false;   // Private plot (non-aircraft related)
ShowSleafordRange = false;  // This shows a range layer based on 53N -0.5W A more reasltic range layer for my antenna location --  AK9T
SleafordMySql = false;      // Don't set this without reviewing the code - it is for me and a local mySql server on 192.168.1.11
//OpenAIPAPIKey = "redacted";
// ----------------------------------------------------------------------------------------------------------------------------

