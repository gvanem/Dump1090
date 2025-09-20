#!/usr/bin/env python3
"""
dump1090 Configuration Setup Script
Updates location coordinates and enables/disables location services in dump1090.cfg
"""

import urllib.request
import urllib.parse
import json
import os
import sys

def query_nominatim(location_query):
    """
    Query Nominatim OpenStreetMap API for coordinates
    """
    # URL encode the query
    encoded_query = urllib.parse.quote_plus(location_query)
    url = f"https://nominatim.openstreetmap.org/search?q={encoded_query}&format=json"

    try:
        print(f"Querying: {url}")
        with urllib.request.urlopen(url) as response:
            data = json.loads(response.read().decode())

        if not data:
            print("No results found for that location.")
            return None

        # Return the first result
        result = data[0]
        lat = float(result['lat'])
        lon = float(result['lon'])
        display_name = result.get('display_name', 'Unknown location')

        print(f"Found: {display_name}")
        print(f"Coordinates: {lat}, {lon}")

        return lat, lon

    except Exception as e:
        print(f"Error querying location: {e}")
        return None

def read_config_file(filename):
    """
    Read the configuration file and return lines
    """
    try:
        with open(filename, 'r') as f:
            return f.readlines()
    except FileNotFoundError:
        print(f"Config file '{filename}' not found.")
        return None
    except Exception as e:
        print(f"Error reading config file: {e}")
        return None

def write_config_file(filename, lines):
    """
    Write lines back to the configuration file
    """
    try:
        with open(filename, 'w') as f:
            f.writelines(lines)
        return True
    except Exception as e:
        print(f"Error writing config file: {e}")
        return False

def update_config_line(lines, key, value):
    """
    Update or add a configuration line
    """
    updated = False

    for i, line in enumerate(lines):
        # Strip whitespace and check if line starts with the key
        stripped = line.strip()
        if stripped and not stripped.startswith('#'):
            # Split on first '=' to handle key = value format
            parts = stripped.split('=', 1)
            if len(parts) == 2:
                config_key = parts[0].strip()
                if config_key == key:
                    # Preserve original spacing style if possible
                    if '=' in line:
                        # Find the = position and replace everything after it
                        eq_pos = line.find('=')
                        # Keep original indentation and spacing before =
                        new_line = line[:eq_pos + 1] + f" {value}\n"
                        lines[i] = new_line
                        updated = True
                        break

    # If key wasn't found, add it at the end
    if not updated:
        lines.append(f"{key} = {value}\n")

    return lines

def main():
    config_file = "dump1090.cfg"

    print("=== dump1090 Configuration Setup ===\n")

    # Check if config file exists
    if not os.path.exists(config_file):
        print(f"Config file '{config_file}' not found in current directory.")
        print("Please make sure you're running this script from the dump1090 directory.")
        sys.exit(1)

    # Get location query from user
    location_query = input("Enter your location (e.g., 'Shorewood MN' or '123 Main St, City State'): ").strip()

    if not location_query:
        print("No location entered. Exiting.")
        sys.exit(1)

    # Query Nominatim for coordinates
    coordinates = query_nominatim(location_query)

    if coordinates is None:
        print("Failed to get coordinates. Exiting.")
        sys.exit(1)

    lat, lon = coordinates

    # Ask about location services
    print("\n" + "="*50)
    enable_location = input("Enable location services? (y/n): ").strip().lower()
    location_setting = "true" if enable_location in ['y', 'yes'] else "false"

    # Read config file
    lines = read_config_file(config_file)
    if lines is None:
        sys.exit(1)

    # Update homepos
    print(f"\nUpdating homepos to: {lat},{lon}")
    lines = update_config_line(lines, "homepos", f"{lat},{lon}")

    # Update location setting
    print(f"Setting location services to: {location_setting}")
    lines = update_config_line(lines, "location", location_setting)

    # Write config file back
    if write_config_file(config_file, lines):
        print(f"\nConfiguration updated successfully in '{config_file}'!")
        print(f"Home position: {lat},{lon}")
        print(f"Location services: {location_setting}")
    else:
        print("Failed to update configuration file.")
        sys.exit(1)

if __name__ == "__main__":
    main()
