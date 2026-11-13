import json,threading,unittest,urllib.request,urllib.error
import app
class LocalApiTests(unittest.TestCase):
 def setUp(self):
  self.state=app.State();self.server=app.ThreadingHTTPServer(('127.0.0.1',0),app.handler(self.state));self.worker=threading.Thread(target=self.server.serve_forever);self.worker.start();self.base=f'http://127.0.0.1:{self.server.server_port}';self.addCleanup(self.close)
 def close(self):self.server.shutdown();self.server.server_close();self.worker.join()
 def post(self,token=None,path='/status',data=None):
  return urllib.request.urlopen(urllib.request.Request(self.base+path,data=json.dumps(data or {}).encode(),headers={'Content-Type':'application/json','X-CitroWaker-Token':token or ''}))
 def test_ui_is_local_and_no_arbitrary_file_endpoint(self):
  with urllib.request.urlopen(self.base+'/') as r:s=r.read().decode();self.assertIn('Prepare your SD files.',s);self.assertNotIn('https://',s)
  with self.assertRaises(urllib.error.HTTPError) as e:urllib.request.urlopen(self.base+'/prepare.py')
  self.assertEqual(e.exception.code,404)
 def test_status_requires_session(self):
  with self.assertRaises(urllib.error.HTTPError) as e:self.post()
  self.assertEqual(e.exception.code,403)
  with self.post(self.state.token) as r:self.assertFalse(json.load(r)['running'])
 def test_reject_non_object_request(self):
  request=urllib.request.Request(self.base+'/status',data=b'[]',headers={'X-CitroWaker-Token':self.state.token})
  with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(request)
  self.assertEqual(error.exception.code,400)
 def test_cancel_unknown_picker_and_bad_start(self):
  with self.post(self.state.token,'/cancel') as r:self.assertTrue(json.load(r)['ok'])
  self.assertTrue(self.state.cancel.is_set())
  for route,data in [('/pick',{'kind':'secrets'}),('/start',{})]:
   with self.assertRaises(urllib.error.HTTPError) as e:self.post(self.state.token,route,data)
   self.assertEqual(e.exception.code,400)
if __name__=='__main__':unittest.main()
