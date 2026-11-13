import numpy as np, pandas as pd

df = pd.read_csv('out/fall_fwd_s01_wai.csv', comment='#')
df = df[df.iloc[:, 0].astype(str).str.isdigit()].astype(float)
df.columns = ['t_us','ax','ay','az','gx','gy','gz','mx','my','mz','mag_ok']

amag = np.sqrt(df.ax**2 + df.ay**2 + df.az**2) / 4096.0    # ±8g: 4096 LSB/g
mmag = np.sqrt(df.mx**2 + df.my**2 + df.mz**2) / 30.0      # ±8G: 30 LSB/µT

print('采样率      %.1f Hz'  % (1e6 / df.t_us.diff().median()))   # 期望 50.0
print('静止 |a|    %.3f g'   % amag[amag.between(0.8, 1.2)].median())  # 期望 1.000
print('峰值 |a|    %.2f g'   % amag.max())                        # >7.9 说明削顶
print('磁模值      %.1f µT'  % mmag.median())                     # 期望 25~65
print('mag_ok 比例 %.2f'     % df.mag_ok.mean())                  # 期望 >=0.95