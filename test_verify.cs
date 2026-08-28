function zverify(){
  %id = 0;
  GMH_MissionType.onSelect(%id, "");
  echo("SIZE=" @ GMH_MissionList.size());
  echo("RO0=" @ GMH_MissionList.getRowTextById(0));
  echo("TYPES=" @ GMH_MissionType.size());
}
zverify();
