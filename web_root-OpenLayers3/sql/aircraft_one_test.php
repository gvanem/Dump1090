<?php 

  //--------------------------------------------------------------------------
  // 1) Connect to mysql database
  //--------------------------------------------------------------------------

  include 'sql_server.php';
  include 'sql_table_aircraft.php';

  $con = mysql_connect($host,$user,$pass);
  $dbs = mysql_select_db($databaseName, $con);

  $i = $_GET["icao"];
 
  //--------------------------------------------------------------------------
  // 2) Query database for data
  //--------------------------------------------------------------------------
  //$qry = "SELECT priority, actype, class, seen, aircraft FROM $tableName WHERE icao LIKE '$i' " ;
  $qry = "SELECT UserBool1, ICAOTypeCode, UserString1, UserInt1, Type, UserString2 FROM $tableName WHERE ModeS LIKE '$i' " ;

  $result = mysql_query($qry); 
  $array = mysql_fetch_row($result);  

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