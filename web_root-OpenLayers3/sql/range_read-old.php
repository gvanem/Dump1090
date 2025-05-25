<?php
//----------------------------------------------------------
// 1) Connect to mysql database
//----------------------------------------------------------
include 'sql_server.php';
include 'sql_table_range.php';
$myCon = new mysqli($host, $user, $pass, $databaseName);
//$i = $_GET["r"];
//----------------------------------------------------------
// 2) Query database for data
//----------------------------------------------------------
$myquery = "SELECT * FROM $tableName";
$result = $myCon->query($myquery);
//----------------------------------------------------------
// 3) echo result as json 
//----------------------------------------------------------
//echo json_encode($array);
//----------------------------------------------------------
// or 4) Multiple rows
//----------------------------------------------------------
for ($row_no = 0; $row_no < $result->num_rows; $row_no++) {
    $result->data_seek($row_no);
    $row = $result->fetch_assoc();
    $myData[] = $row;
}
echo json_encode($myData);
?>