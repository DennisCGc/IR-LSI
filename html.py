index = """<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>IR-LSI Interface</title>
    <script type="text/javascript" src="http://code.jquery.com/jquery-1.7.1.min.js"></script>
    <script type="text/javascript">
function getQuerystring(key, default_)
{
  if (default_==null) default_="";
  key = key.replace(/[\[]/,"\\\[").replace(/[\]]/,"\\\]");
  var regex = new RegExp("[\\?&]"+key+"=([^&#]*)");
  var qs = regex.exec(window.location.href);
  if(qs == null)
    return default_;
  else
    return qs[1];
}

function renderResults(data) {
    for (var id in data) {
        html = '<a href =\"http://en.wikipedia.org/wiki/'+data[id][0]+'">'+data[id][0]+'</a></br>';
        $('#div').append(html);
    }
}

query = getQuerystring('q')
if (query != ''){
    $.ajax({url: "http://www.opentripplanner.nl:5678/api/?query="+query, success: renderResults, dataType: "json"});
}
    </script>
  </head>
  <body>
    <form>
      <input type="text" name="q" /><input type="submit" value="Zoeken" />
    </form>
     <div id="div"></div>
  </body>
</html>"""
