<?php 

  include 'sql_server.php';
  include 'sql_table_finds.php';


  $con 		= mysql_connect($host,$user,$pass);
  $dbs 		= mysql_select_db($databaseName, $con);

  //$result 	= mysql_query("SELECT Lat, Long FROM $tableName WHERE Lat IS NOT NULL ORDER BY Long");
  $result 	= mysql_query("SELECT * FROM $tableName ORDER BY Name");

  $array 	= mysql_fetch_row($result);                             

  $data 	= array();

  //echo json_encode($array);

  while ( $row = mysql_fetch_row($result) )
  {
    	$data[] = $row;
  }
  echo json_encode( $data );
?>