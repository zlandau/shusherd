import sys
import argparse
from subprocess import Popen
import time
import os
import atexit
import json
import requests


# XXX: always kill process

SHUSHER_HELPER = './shusherd'
SHUSHER_CONFIG = 'shusherrc'

child_process = None


@atexit.register
def cleanup_child():
    if child_process:
        child_process.kill()


class Shusher(object):
    def __init__(self, args):
        self.host = args.host
        self.mac_addr = args.mac_addr

    def run(self):
        config = self.get_config()
        self.write_config(config)
        while True:
            child_process = Popen([SHUSHER_HELPER])
            reload = False
            while not reload:
                new_cfg = self.get_config()
                if config != new_cfg:
                    config = new_cfg
                    self.write_config(config)
                    child_process.terminate()
                    reload = True
                else:
                    time.sleep(new_cfg['poll_interval'])
            print("return code", child_process.returncode)
            print(child_process.returncode)

    def get_config(self):
        r = requests.post('http://{}/config'.format(self.host), json={
            "mac": self.mac_addr
        })
        if r.status_code() == 200:
            return r.json()

        #print(json.load(open("config.json")))
        #return json.load(open("config.json"))

    def write_config(self, cfg):
        print("writing a config", cfg)
        tmpcfg = SHUSHER_CONFIG + '.tmp'
        with open(tmpcfg, 'w') as f:
            f.write('decay = {:.2}\n'.format(cfg['decay']))
            f.write('threshold = {}\n'.format(cfg['threshold']))
            f.write('verbosity = {}\n'.format(cfg['verbosity']))
            f.write('input_file = "{}"\n'.format(cfg['input_file']))
        os.rename(tmpcfg, SHUSHER_CONFIG)


def main(argv):
    parser = argparse.ArgumentParser(
        prog='shusherd',
        description='Shusher daemon')
    parser.add_argument('-H', '--host', required=True)
    parser.add_argument('-M', '--mac-addr', required=True)
    parser.add_argument('-f', '--foreground', action='store_true')

    args = parser.parse_args(argv[1:])

    shusher = Shusher(args)
    shusher.run()

if __name__ == '__main__':
    sys.exit(main(sys.argv))