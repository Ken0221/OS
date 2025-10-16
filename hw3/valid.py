# 讀取 output_{n}.txt, n = 1 ~ 8
# 檢查是否與 output_correct.txt 相同，並輸出每個檔案的比較結果
import os
import filecmp

for i in range(1, 9):
    output_file = f'output_{i}.txt'
    correct_file = 'output_correct_100000.txt'
    
    if os.path.exists(output_file):
        if filecmp.cmp(output_file, correct_file, shallow=False):
            print(f'{output_file} is correct.')
        else:
            print(f'{output_file} is incorrect.')
    else:
        print(f'{output_file} does not exist.')
