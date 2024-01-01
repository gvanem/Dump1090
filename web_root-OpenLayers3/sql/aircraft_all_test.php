<?php 

  include 'sql_server.php';
  include 'sql_table_aircraft.php';


  $con 		= mysql_connect($host,$user,$pass);
  $dbs 		= mysql_select_db($databaseName, $con);

  $result 	= mysql_query("SELECT ModeS, Type FROM $tableName WHERE Type IS NOT NULL ORDER BY ModeS");
  $array 	= mysql_fetch_row($result);                             

  $data 	= array();

  while ( $row = mysql_fetch_row($result) )
  {
    	$data[] = $row;
  }
  echo json_encode( $data );
?>