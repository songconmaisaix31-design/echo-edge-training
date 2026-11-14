import os, tempfile, sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parent/'backend'))
root=Path(tempfile.mkdtemp(prefix='echo-exam-test-'))
os.environ['ECHO_DATABASE_URL']='sqlite:///'+str(root/'test.db')
os.environ['ECHO_MEDIA_ROOT']=str(root/'media')
import main, platform_db as p
p.Base.metadata.create_all(p.engine)
with p.Session() as db:
 salt='12'*16
 db.add(p.User(name='test-doctor',role='operator',password=salt+':'+p.digest('test-password-123',salt)))
 db.commit()
import uvicorn
uvicorn.run(main.app,host='127.0.0.1',port=8005)
