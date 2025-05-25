<?php
include 'sql_server.php';
//include 'sql_table_range.php';
$databaseName = "AllanK";
$tableName = "ADSBRange";

$myCon = new mysqli($host, $user, $pass, $databaseName);

$ty = $_GET["ring"];
$bg = $_GET["bearing"];
$rg = $_GET["range"];
$lt = $_GET["lat"];
$ln = $_GET["lon"];
$ic = $_GET["icao"];
$fl = $_GET["fltlvl"];

$myquery = "UPDATE $tableName SET `adsbRange` = $rg, `adsbLat` = $lt, `adsbLong` = $ln, `adsbIcao` = '$ic', `adsbUpdated` = CURRENT_TIMESTAMP WHERE `bearing` = $bg AND `adsbRange` < $rg";

$myCon->query($myquery);

//$result = $myCon->query($myquery);
//echo "<script type='text/javascript'>alert('$result ');</script>";

//if ($myCon->query($myquery) === TRUE) {
//$result = "Record updated successfully";
//} else {
//$result = "Error updating record: " . $myCon->error;
//}

//$result = $myCon->query($myquery);

//$result = $myCon->error;

$myCon->close();
?>