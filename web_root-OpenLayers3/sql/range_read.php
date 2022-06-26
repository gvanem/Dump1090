<?php 

  //----------------------------------------------------------
  // 1) Connect to mysql database
  //----------------------------------------------------------

  include 'sql_server.php';
  include 'sql_table_range.php';

  $con = mysql_connect($host,$user,$pass);
  $dbs = mysql_select_db($databaseName, $con);

 
  //----------------------------------------------------------
  // 2) Query database for data
  //----------------------------------------------------------
  $qry = "SELECT `bearing`, `range` FROM $tableName WHERE 1 ORDER BY `range` DESC" ;

  $result = mysql_query($qry); 
  $array  = mysql_fetch_row($result);  

  //----------------------------------------------------------
  // 3) echo result as json 
  //----------------------------------------------------------
  //echo json_encode($array);
  //----------------------------------------------------------
  // or 4) Multiple rows
  //----------------------------------------------------------
  while ( $row = mysql_fetch_row($result) )
  {
    $rngData[] = $row; 
  }
  echo json_encode( $rngData );

?>