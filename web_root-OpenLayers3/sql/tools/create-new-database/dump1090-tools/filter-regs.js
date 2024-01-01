//
// This script processes a CSV file that contains
// ICAO addresses (column 'icao24') and registrations
// (column 'r')
//
// It removes all registration entries that exactly match
// what dump1090 would have computed from the hexid anyway,
// reducing the size of the CSV in the cases where the
// two approaches match.
//
// Any additional columns are passed through unchanged.
//
// To run it:
//
//   sudo apt-get install nodejs
//   sudo apt-get install npm
//   npm install csv     # must be done in the same dir as this script
//   nodejs filter-regs.js <input.csv >output.csv

var reglookup = require('../public_html/registrations.js');
var csv = require('csv');

var parser = csv.parse({columns: true});
var writer = csv.stringify({header: true});
var transformer = csv.transform(function (record, callback) {
        if (('icao24' in record) && ('r' in record)) {
                var computed = reglookup(record.icao24);
                if (computed === record.r) {
                        record.r = '';
                } else if (computed !== null) {
                        console.warn(record.icao24 + " computed " + computed + " but CSV data had " + record.r);
                }
        }

        callback(null, record);
});

process.stdin.pipe(parser).pipe(transformer).pipe(writer).pipe(process.stdout);
