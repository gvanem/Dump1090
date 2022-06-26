<?php 

  //--------------------------------------------------------------------------
  // 1) Connect to mysql database
  //--------------------------------------------------------------------------

  include 'sql_server.php';
  include 'sql_table_seen.php';

  $con = mysql_connect($host,$user,$pass);
  $dbs = mysql_select_db($databaseName, $con);

 
  //--------------------------------------------------------------------------
  // 2) Query database for data
  //--------------------------------------------------------------------------

  $qry = "SELECT `icao`,`firstseen`,`lastseen`,`seen` from $tableName  WHERE `seen` > 0 ORDER BY `seen` DESC" ;

  $result = mysql_query($qry); 
  $array = mysql_fetch_row($result);  

  //--------------------------------------------------------------------------
  // 3) echo result as json 
  //--------------------------------------------------------------------------
  while ( $row = mysql_fetch_row($result) )
  {
    	$data[] = $row;
  }
  echo json_encode( $data );
?>