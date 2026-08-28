function zdump(){
  %t = 0;
  while(%t < $HostTypeCount){
    echo("T["@ %t @ "]=" @ $HostTypeName[%t] @ " C=[" @ $HostMissionCount[%t] @ "]");
    %t = %t + 1;
  }
  %ci = 0;
  %t = 0;
  while(%t < $HostTypeCount){
    if($HostTypeName[%t] $= "ctf") {%ci = %t;}
    %t = %t + 1;
  }
  echo("CTF IDX=" @ %ci);
  GMH_MissionType.onSelect(%ci, "");
  echo("LIST SIZE=" @ GMH_MissionList.size());
}
zdump();
