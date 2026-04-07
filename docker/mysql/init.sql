USE qgydb;

CREATE TABLE IF NOT EXISTS user (
    username CHAR(50) NULL,
    passwd CHAR(50) NULL
) ENGINE=InnoDB;

INSERT INTO user(username, passwd)
SELECT 'name', 'passwd'
WHERE NOT EXISTS (
    SELECT 1 FROM user WHERE username = 'name' AND passwd = 'passwd'
);
