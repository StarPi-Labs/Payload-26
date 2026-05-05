% Parametri di Volo EuRoC - MPU6050
% Temporizzazione
fs = 1000; % Frequenza di campionamento(Hz)
Ts = 1/fs; % Sample Time(0.001s)

% Soglie
threshold_sigma = 0.1; % Soglia di Tolleranza per la calibrazione
Liftoff_threshold = 2; % Massimo movimento calibrating (m/s^2 ?)
Time = 10; % Tempo per conferma threshold
threshold_watchdog = 100; % dopo quanti cicli considero il sensore bloccato

% Costanti Fisiche
gravity = 1; % Accelerazione di Gravità