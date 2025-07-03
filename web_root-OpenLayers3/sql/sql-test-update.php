<?php
include 'sql_server.php';
//include 'sql_table_range.php';
$databaseName = "AllanK";
$tableName = "ADSBRange";

$myCon = new mysqli($host, $user, $pass, $databaseName);
// Check connection
if ($myCon->connect_error) {
    die("Connection failed: " . $myCon->connect_error);
}

// works: http://192.168.1.18/dump1090/sql/sql-test-update.php?bearing=3&range=2&lat=53.1&lon=-0.4

$bg = $_GET["bearing"];
$rg = $_GET["range"];
$lt = $_GET["lat"];
$ln = $_GET["lon"];
$ic = $_GET["icao"];

$sql = "UPDATE $tableName SET `adsbRange` = $rg, `adsbLat` = $lt, `adsbLong` = $ln, `adsbIcao` = '$ic' WHERE `bearing` = $bg";
//$sql = "UPDATE ADSBRange SET `adsbRange` = 1, `adsbLat` = 53.1, `adsbLong` = -0.4, `adsbIcao` = '123456' WHERE `bearing` = 1";

if ($myCon->query($sql) === TRUE) {
    echo "Record updated successfully";
} else {
    echo "Error updating record: " . $myCon->error;
}

$myCon->close();
?>