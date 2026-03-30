from __future__ import annotations

from dataclasses import dataclass
from typing      import Optional
from pprint      import pprint

import config
from utils import calculate_distance_bearing

@dataclass
class Aircraft:
  """Aircraft data from tar1090"""
  hex_code: str
  callsign: str
  lat: float
  lon: float
  altitude: int
  speed: int
  track: float
  distance: float
  bearing: float
  is_military: bool = False
  on_ground:   bool = False

  @staticmethod
  def from_dict (data: dict) -> Optional[Aircraft]:
    """Create an Aircraft object from a dictionary."""
    if "lat" not in data or "lon" not in data:
       return None

    lat = data ["lat"]
    lon = data ["lon"]
    distance, bearing = calculate_distance_bearing (config.LAT, config.LON, lat, lon)
    if distance > config.RADIUS_NM:
       return None

    hex_code = data ["hex"].lower()
    mil_prefixes = tuple (prefix.lower() for prefix in config.MIL_PREFIX_LIST)
    is_military  = hex_code.startswith (mil_prefixes)

    _altitude = _speed = 0

    if "altitude" in data:
       _altitude = data.get ("altitude", 0) or 0
    elif "alt_baro" in data:
       _altitude = data.get ("alt_baro", 0) or 0

    if "gs" in data:
       _speed = data.get ("gs", 0) or 0
    elif "speed" in data:
       _speed = data.get ("speed", 0) or 0

    if config.VERBOSE:
       print (f"_altitude: {_altitude}")

    return Aircraft (
        hex_code = hex_code,
        callsign = data.get("flight", "UNKNOWN").strip()[:8],
        lat = lat, lon = lon,
        altitude = _altitude,
        speed    = _speed,
        track    = data.get ("track", 0) or 0,
        distance = distance, bearing = bearing,
        is_military = is_military,
        on_ground   = _altitude == "ground"
      )