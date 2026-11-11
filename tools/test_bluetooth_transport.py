import struct,time,unittest
from types import SimpleNamespace
from unittest.mock import Mock,patch
from bluetooth_transport import BluetoothSerial,BluetoothConnection,FragmentAssembler,FRAGMENT,TX_UUID,RX_UUID,SERVICE_UUID

def packet(kind=b'BSMT',body=b'{"temperature_f":80}'):
 return kind+struct.pack('>I',len(body))+body

def fragments(data,frame=1,width=17):
 return [FRAGMENT.pack(b'\xb7M',frame,i,len(data))+data[i:i+width] for i in range(0,len(data),width)]

class FragmentTests(unittest.TestCase):
 def test_all_packets_at_small_and_large_mtu(self):
  for kind,body in ((b'BSMT',b'{}'),(b'BSMC',b'x'*9999),(b'BSMP',b'{}'),(b'BSMF',b'x'*20000),(b'BSMH',b'25.0,'*768),(b'BSMD',bytes(5))):
   for width in (8,232,499):
    reader=FragmentAssembler();results=[reader.feed(f) for f in fragments(packet(kind,body),width=width)]
    self.assertEqual([r for r in results if r is not None],[packet(kind,body)])
 def test_aborted_image_then_sensor_recovers(self):
  reader=FragmentAssembler();image=fragments(packet(b'BSMF',b'x'*1000),frame=2)
  self.assertIsNone(reader.feed(image[0]))
  results=[reader.feed(f) for f in fragments(packet(),frame=3)]
  self.assertEqual(results[-1],packet());self.assertIsNone(reader.feed(image[1]))
 def test_lost_reordered_and_bad_fragments(self):
  for indexes in ((0,2,3),(0,1,1,2,3),(1,0,2,3)):
   reader=FragmentAssembler();parts=fragments(packet(body=b'x'*60))
   self.assertFalse(any(reader.feed(parts[i]) is not None for i in indexes))
  for value in (b'x',FRAGMENT.pack(b'XX',1,0,10)+b'xxx',FRAGMENT.pack(b'\xb7M',1,0,200009)+b'x',FRAGMENT.pack(b'\xb7M',1,12,10)+b'x'):
   self.assertIsNone(FragmentAssembler().feed(value))
  for value in (packet(b'FAKE'),b'BSMT'+struct.pack('>I',9)+b'{}',packet(b'BSMD',bytes(6))):
   self.assertIsNone(FragmentAssembler().feed(FRAGMENT.pack(b'\xb7M',1,0,len(value))+value))

class FakeScanner:
 @staticmethod
 async def find_device_by_filter(callback,**kwargs):
  dev=SimpleNamespace(name='Backseat Minder',address='test')
  assert callback(dev,SimpleNamespace(service_uuids=[SERVICE_UUID],local_name='Backseat Minder'))
  return dev
 @staticmethod
 async def find_device_by_address(address,**kwargs):return SimpleNamespace(name='Backseat Minder',address=address)
class FakeClient:
 instances=[];fail_write=False
 def __init__(self,device,**kwargs):
  self.kwargs=kwargs;self.is_connected=False;self.writes=[];self.instances.append(self)
 async def __aenter__(self):self.is_connected=True;return self
 async def __aexit__(self,*args):self.is_connected=False;self.kwargs['disconnected_callback'](self)
 async def start_notify(self,uuid,callback):
  assert uuid==TX_UUID
  for f in fragments(packet()):callback(None,f)
 async def write_gatt_char(self,uuid,data,**kwargs):
  self.writes.append((uuid,data,kwargs))
  if self.fail_write:raise RuntimeError('simulated loss after submission')
def wait_for(predicate):
 until=time.monotonic()+2
 while time.monotonic()<until:
  if predicate():return
  time.sleep(.005)
 raise AssertionError('Timed out waiting for fake BLE connection')
class ConnectionTests(unittest.TestCase):
 def setUp(self):
  FakeClient.instances=[];FakeClient.fail_write=False
  self.device=BluetoothSerial(scanner=FakeScanner,client_factory=FakeClient)
  self.connection=BluetoothConnection(self.device);wait_for(lambda:self.device.in_waiting>0)
 def tearDown(self):self.device.close()
 def test_receive_and_control_use_encrypted_client(self):
  self.assertEqual(self.connection.read_available(0),packet());self.assertEqual(self.connection.generation,1)
  self.assertTrue(FakeClient.instances[0].kwargs['pair'])
  self.assertEqual(self.device.write(b'SEND_SMS\n'),9)
  self.assertEqual(FakeClient.instances[0].writes,[(RX_UUID,b'SEND_SMS\n',{'response':True})])
  with self.assertRaises(OSError):self.device.write(b'ATD933;\n')
 def test_unknown_command_outcome_never_retries(self):
  FakeClient.fail_write=True
  with self.assertRaises(OSError):self.device.write(b'SEND_SMS\n')
  self.assertEqual(len(FakeClient.instances[0].writes),1)
 def test_disconnect_clears_buffer_and_reconnects(self):
  self.connection.read_available(0);client=FakeClient.instances[0];client.is_connected=False;client.kwargs['disconnected_callback'](client)
  self.assertIsNone(self.connection.read_available(1));self.assertEqual(self.device.in_waiting,0)
  wait_for(lambda:len(FakeClient.instances)>1 and self.device.in_waiting>0)
  self.assertEqual(self.connection.read_available(2),packet());self.assertEqual(self.connection.generation,2)
 def test_overflow_changes_generation(self):
  self.connection.read_available(0);before=self.device.session_id
  for i in range(30):
   for f in fragments(packet(b'BSMF',b'x'*20000),frame=i):self.device._notify(None,f)
  self.assertLessEqual(self.device.in_waiting,512000);self.assertGreater(self.device.session_id,before)
class CliTests(unittest.TestCase):
 def test_bluetooth_flag_never_opens_usb(self):
  import usb_telemetry_viewer as viewer
  device=Mock()
  with patch('sys.argv',['viewer','--bluetooth']),patch('bluetooth_transport.BluetoothSerial',return_value=device) as constructor,patch.object(viewer,'show_viewer',return_value=0),patch.object(viewer.serial,'Serial') as usb,patch('builtins.print'):
   self.assertEqual(viewer.main(),0);constructor.assert_called_once_with(None,'Backseat Minder');usb.assert_not_called();device.close.assert_called_once()
if __name__=='__main__':unittest.main()
