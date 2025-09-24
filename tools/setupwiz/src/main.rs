/*!
 * dump1090 Configuration Setup
 * Updates location coordinates and enables/disables location services in dump1090.cfg
 */

use serde::Deserialize;
use std::fs;
use std::io::{self, Write};
use std::process;

#[derive(Debug, Deserialize)]
struct NominatimResult {
    lat: String,
    lon: String,
    display_name: Option<String>,
}

/// Query Nominatim OpenStreetMap API for coordinates
fn query_nominatim(location_query: &str) -> Result<(f64, f64), Box<dyn std::error::Error>> {
    // URL encode the query
    let encoded_query = urlencoding::encode(location_query);
    let url = format!(
        "https://nominatim.openstreetmap.org/search?q={}&format=json",
        encoded_query
    );

    println!("Querying: {}", url);

    let client = reqwest::blocking::Client::new();
    let response = client.get(&url).send()?;
    let results: Vec<NominatimResult> = response.json()?;

    if results.is_empty() {
        println!("No results found for that location.");
        return Err("No results found".into());
    }

    // Return the first result
    let result = &results[0];
    let lat: f64 = result.lat.parse()?;
    let lon: f64 = result.lon.parse()?;
    let display_name = result.display_name.as_deref().unwrap_or("Unknown location");

    println!("Found: {}", display_name);
    println!("Coordinates: {}, {}", lat, lon);

    Ok((lat, lon))
}

/// Read the configuration file and return lines
fn read_config_file(filename: &str) -> Result<Vec<String>, Box<dyn std::error::Error>> {
    match fs::read_to_string(filename) {
        Ok(content) => Ok(content.lines().map(|line| format!("{}\n", line)).collect()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            println!("Config file '{}' not found.", filename);
            Err(e.into())
        }
        Err(e) => {
            println!("Error reading config file: {}", e);
            Err(e.into())
        }
    }
}

/// Write lines back to the configuration file
fn write_config_file(filename: &str, lines: &[String]) -> Result<(), Box<dyn std::error::Error>> {
    let content = lines.join("");
    match fs::write(filename, content) {
        Ok(_) => Ok(()),
        Err(e) => {
            println!("Error writing config file: {}", e);
            Err(e.into())
        }
    }
}

/// Update or add a configuration line
fn update_config_line(lines: &mut Vec<String>, key: &str, value: &str) {
    let mut updated = false;

    for line in lines.iter_mut() {
        // Strip whitespace and check if line starts with the key
        let stripped = line.trim();
        if !stripped.is_empty() && !stripped.starts_with('#') {
            // Split on first '=' to handle key = value format
            if let Some(eq_pos) = stripped.find('=') {
                let config_key = stripped[..eq_pos].trim();
                if config_key == key {
                    // Preserve original spacing style if possible
                    if let Some(eq_pos_in_line) = line.find('=') {
                        // Keep original indentation and spacing before =
                        let new_line = format!("{} {}\n", &line[..eq_pos_in_line + 1], value);
                        *line = new_line;
                        updated = true;
                        break;
                    }
                }
            }
        }
    }

    // If key wasn't found, add it at the end
    if !updated {
        lines.push(format!("{} = {}\n", key, value));
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let config_file = "dump1090.cfg";

    println!("=== Dump1090 Configuration Setup ===\n");

    // Check if config file exists
    if !std::path::Path::new(config_file).exists() {
        println!(
            "Config file '{}' not found in current directory.",
            config_file
        );
        println!("Please make sure you're running this script from the dump1090 directory.");
        process::exit(1);
    }

    // Get location query from user
    print!("Enter your location (e.g., 'Shorewood MN' or '123 Main St, City State'): ");
    io::stdout().flush()?;

    let mut location_query = String::new();
    io::stdin().read_line(&mut location_query)?;
    let location_query = location_query.trim();

    if location_query.is_empty() {
        println!("No location entered. Exiting.");
        process::exit(1);
    }

    // Query Nominatim for coordinates
    let coordinates = match query_nominatim(location_query) {
        Ok(coords) => coords,
        Err(_) => {
            println!("Failed to get coordinates. Exiting.");
            process::exit(1);
        }
    };

    let (lat, lon) = coordinates;

    // Ask about location services
    println!("\n{}", "=".repeat(50));
    print!("Enable location services? (y/n): ");
    io::stdout().flush()?;

    let mut enable_location = String::new();
    io::stdin().read_line(&mut enable_location)?;
    let enable_location = enable_location.trim().to_lowercase();
    let location_setting = if enable_location == "y" || enable_location == "yes" {
        "true"
    } else {
        "false"
    };

    // Read config file
    let mut lines = match read_config_file(config_file) {
        Ok(lines) => lines,
        Err(_) => process::exit(1),
    };

    // Update homepos
    println!("\nUpdating homepos to: {},{}", lat, lon);
    update_config_line(&mut lines, "homepos", &format!("{},{}", lat, lon));

    // Update location setting
    println!("Setting location services to: {}", location_setting);
    update_config_line(&mut lines, "location", location_setting);

    // Write config file back
    match write_config_file(config_file, &lines) {
        Ok(_) => {
            println!("\nConfiguration updated successfully in '{}'!", config_file);
            println!("Home position: {},{}", lat, lon);
            println!("Location services: {}", location_setting);
        }
        Err(_) => {
            println!("Failed to update configuration file.");
            process::exit(1);
        }
    }

    Ok(())
}
