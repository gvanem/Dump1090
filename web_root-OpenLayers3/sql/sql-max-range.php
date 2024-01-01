<?php 
  include 'sql_server.php';
  include 'sql_table_range.php';

  $con = mysql_connect($host,$user,$pass);
  $dbs = mysql_select_db($databaseName, $con);

  $result = mysql_query("SELECT * FROM $tableName");        
  $array = mysql_fetch_row($result);            
             
  $data = array();

  while ( $row = mysql_fetch_row($result) )
  {
    $data[] = $row;
  }
  echo json_encode( $data );
?>