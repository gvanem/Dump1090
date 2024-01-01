<?php 

  include 'sql_server.php';
  include 'sql_table_range.php';

  $con = mysql_connect($host,$user,$pass);
  $dbs = mysql_select_db($databaseName, $con);
  
  $bg = $_GET["bearing"];
  $rg = $_GET["range"];
  $lt = $_GET["lat"];
  $ln = $_GET["lon"];
  $ic = $_GET["icao"];
  

  $qry = "UPDATE $tableName SET `range` = $rg, `lat` = $lt, `long` = $ln, `icao` = '$ic' WHERE `bearing` = $bg AND `range` < $rg";
  //$qry = "UPDATE $tableName SET `range` = $rg, `lat` = $lt, `long` = $ln WHERE `bearing` = $bg AND `range` < $rg";


  $result = mysql_query($qry); 
 
  //echo json_encode($result);  //usefull as it returns true or false depending on query success
?>