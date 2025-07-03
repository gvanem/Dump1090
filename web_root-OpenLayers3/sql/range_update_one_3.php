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

if ($ty == 'min') {
    $myquery = "UPDATE $tableName SET `minrng` = $rg, `minlat` = $lt, `minlon` = $ln, `minicao` = '$ic', `minfl` = $fl, `minupdated` = CURRENT_TIMESTAMP WHERE `bearing` = $bg AND `minrng` < $rg";
} elseif ($ty == 'mid') {
    $myquery = "UPDATE $tableName SET `midrng` = $rg, `midlat` = $lt, `midlon` = $ln, `midicao` = '$ic', `midfl` = $fl, `midupdated` = CURRENT_TIMESTAMP WHERE `bearing` = $bg AND `midrng` < $rg";
} else {
    $myquery = "UPDATE $tableName SET `maxrng` = $rg, `maxlat` = $lt, `maxlon` = $ln, `maxicao` = '$ic', `maxfl` = $fl, `maxupdated` = CURRENT_TIMESTAMP WHERE `bearing` = $bg AND `maxrng` < $rg";
}

$myCon->query($myquery);

$myCon->close();
?>