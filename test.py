import redis
import time
from difflib import SequenceMatcher
import random

def get_string_diff(str1, str2):
    matcher = SequenceMatcher (None, str1, str2)
    diff = ""
    for op, i1, i2, j1, j2 in matcher.get_opcodes():
        if op == 'replace':
            diff += f"{str1[i1:i2]} →> {str2[j1:j2]}\n"
        elif op == 'delete':
            diff += f"-{str1[i1:i2]}\n"
        elif op == 'insert':
            diff += f"+{str2[j1:j2]}\n"
    return diff

def generate_random_str(randomlength=16):
    random_str = ''
    base_str = 'ABCDEFGHIGKLMNOPQRSTUVWXYZabcdefghigklmnopqrstuvwxyz123456789'
    length = len(base_str) - 1
    for i in range(randomlength):
        random_str += base_str[random.randint(0, length)]
    return random_str


# keyLength = 1024 * 8
key = "randomKey"
# digits = "0123456789"
# num_digits = len(digits)
# value = ""
# for i in range(keyLength):
#     value += digits[i % num_digits]
value = generate_random_str(1024*32)

# print(value)

r = redis.Redis(host='localhost', port=6380, db=0, decode_responses=True)

# print(value)

print(r.set(key, value))

# time.sleep(2)

ret = r.get(key)
# print(ret)

print("The test result is", ret == value)

print(get_string_diff(value, ret))