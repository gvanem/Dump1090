<?php 

  //--------------------------------------------------------------------------
  // 1) Connect to mysql database
  //--------------------------------------------------------------------------

  include 'sql_server.php';
  include 'sql_table_seen.php';

  $con = mysql_connect($host,$user,$pass);
  $dbs = mysql_select_db($databaseName, $con);

  $i = $_GET["icao"];
 
  //--------------------------------------------------------------------------
  // 2) Query database for data
  //--------------------------------------------------------------------------

  $qry = "SELECT seen FROM $tableName WHERE icao LIKE '$i' " ;

  $result = mysql_query($qry); 
  $array = mysql_fetch_row($result);  
  //--------------------------------------------------------------------------
  // UserBool1   = Interset         UserInt1    = times seen
  // USerString1 = type (for image) USerString2 = type (shortened)
  // Type        = type (ICAO)
  //--------------------------------------------------------------------------

  //--------------------------------------------------------------------------
  // 3) echo result as json 
  //--------------------------------------------------------------------------
  echo json_encode($array);

  //--------------------------------------------------------------------------
  // or 4) Multiple rows
  //--------------------------------------------------------------------------
  //$data = array();
  //while ( $row = mysql_fetch_row($result) )
  //{
  //  $acData[] = $row; 
  //}
  //echo json_encode( $acData );

?>